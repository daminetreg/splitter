#include <clang-c/Index.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cassert>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include <boost/process.hpp>

namespace fs = std::filesystem;

static std::vector<std::string> detect_system_includes(const std::string& compiler = "g++") {
    std::vector<std::string> includes;
    std::string cmd = compiler + " -E -x c++ /dev/null -v 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return includes;

    char buf[4096];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe))
        output += buf;
    pclose(pipe);

    bool in_search_list = false;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("#include <...> search starts here:") != std::string::npos) {
            in_search_list = true;
            continue;
        }
        if (line.find("End of search list.") != std::string::npos)
            break;
        if (in_search_list && !line.empty() && line[0] == ' ') {
            std::string path = line.substr(1);
            while (!path.empty() && (path.back() == ' ' || path.back() == '\n'))
                path.pop_back();
            size_t paren = path.find(" (");
            if (paren != std::string::npos)
                path = path.substr(0, paren);
            if (!path.empty() && fs::exists(path))
                includes.push_back(path);
        }
    }
    return includes;
}

// The C++ standard this unit is being compiled at, as the __cplusplus value, or 0 when it
// could not be determined. Set once per split from the same flags libclang is given, so the
// splitter's decisions and the compiler's rules cannot disagree.
static int g_cxx_standard = 0;

// "-std=gnu++17" -> 201703. Returns 0 for anything unrecognised, which callers treat as
// "assume the older behaviour", because guessing high is the unsafe direction.
static int standard_from_flag(const std::string& flag) {
    const size_t plus = flag.rfind("++");
    if (plus == std::string::npos) return 0;
    const std::string ver = flag.substr(plus + 2);
    static const std::pair<const char*, int> known[] = {
        {"98", 199711}, {"03", 199711},
        {"11", 201103}, {"0x", 201103},
        {"14", 201402}, {"1y", 201402},
        {"17", 201703}, {"1z", 201703},
        {"20", 202002}, {"2a", 202002},
        {"23", 202302}, {"2b", 202302},
    };
    for (const auto& k : known)
        if (ver == k.first) return k.second;
    return 0;
}

enum class ScopeKind { Namespace, Class };

struct ScopeEntry {
    std::string name;
    ScopeKind kind;
};

struct FunctionInfo {
    std::string name;
    std::string qualified_name;
    std::string return_type;
    std::string signature;
    std::string body;
    unsigned start_line;
    unsigned end_line;
    unsigned start_offset;
    unsigned end_offset;
    std::string file;                 // absolute path of the file holding the definition
    std::string usr;                  // unified symbol name, for the referenced-set lookup
    std::vector<ScopeEntry> scope_chain;
    bool is_template;
    bool is_static;

    // Set by prepare_functions() once the whole file has been visited.
    bool is_constexpr = false;        // constexpr/consteval, however it was spelled
    bool is_specialization = false;   // explicit specialization: `template< > void f<T>()`
    bool is_virtual = false;          // virtual member function
    bool is_ctor_or_dtor = false;     // constructor or destructor
    bool is_conversion = false;       // `operator T()`: no return type to rebuild it from
    // Whether a definition may legally appear in many translation units. Everything the
    // preamble carries is included by every piece, so only a vague-linkage definition can
    // be left in it; anything else has to appear exactly once.
    bool is_inlined = false;          // inline, explicitly or by being defined in-class
    bool external_linkage = false;
    // Source ranges of always_inline attributes on this definition, as file offsets. An
    // always-inline function is given available_externally linkage and never emitted out
    // of line, so nothing can ever link against it; wherever such a definition is
    // re-emitted the attribute is dropped so that a real body exists.
    std::vector<std::pair<unsigned, unsigned>> always_inline_ranges;
    std::vector<std::string> conditionals;   // #if directives active at the definition
    bool defined_in_class = false;    // body written inside the class, not already out-of-line
    unsigned unnamed_ns_depth = 0;    // innermost consecutive unnamed namespaces around it
    bool in_unnamed_ns = false;       // an unnamed namespace encloses it at any depth
    bool in_class_template = false;   // member of a class template: cannot go out-of-line
    bool in_anonymous_class = false;  // no class name to qualify a definition with
    // The function's type, kept so that the linkage of the types in its signature can be
    // examined later. Valid only while the translation unit it came from is alive, which is
    // exactly as long as prepare_functions() runs.
    CXType fn_type{CXType_Invalid, {nullptr, nullptr}};
    // A type in the signature has no linkage, so the definition cannot leave this unit.
    bool signature_lacks_linkage = false;
    // Member of a class-template specialization whose name libclang cannot spell.
    bool in_specialization_without_name = false;
    bool keep_in_header = false;      // definition has to stay in the preamble
    // Another definition reports the same extent: the mark of a macro that expands to more
    // than one declaration, whose extent is the invocation rather than any one declarator.
    bool shares_extent = false;
    // The extent was a fragment of a macro invocation and has been widened to the whole
    // invocation, which can then be moved as a unit.
    bool macro_invocation = false;
    bool uses_undefined_macro = false;  // body or conditionals need a macro the file #undefs
    std::string member_decl;          // in-class declaration left behind (members only)
    std::string outlined_body;        // `T Class::name(args) { ... }` (members only)
};

static std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << "Error: cannot open file: " << path << "\n";
        return "";
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static std::string get_source_range_text(CXTranslationUnit tu, CXSourceRange range) {
    CXFile file;
    unsigned start_offset, end_offset;

    CXSourceLocation start_loc = clang_getRangeStart(range);
    CXSourceLocation end_loc = clang_getRangeEnd(range);

    clang_getFileLocation(start_loc, &file, nullptr, nullptr, &start_offset);
    clang_getFileLocation(end_loc, nullptr, nullptr, nullptr, &end_offset);

    if (!file || end_offset <= start_offset)
        return "";

    size_t size = 0;
    const char* buffer = clang_getFileContents(tu, file, &size);
    if (!buffer || end_offset > size)
        return "";

    return std::string(buffer + start_offset, end_offset - start_offset);
}

static std::string cx_to_string(CXString s) {
    const char* cstr = clang_getCString(s);
    std::string result = cstr ? cstr : "";
    clang_disposeString(s);
    return result;
}

static std::vector<unsigned> build_line_offsets(const std::string& source) {
    std::vector<unsigned> offsets;
    offsets.push_back(0);
    for (unsigned i = 0; i < source.size(); ++i) {
        if (source[i] == '\n')
            offsets.push_back(i + 1);
    }
    return offsets;
}

static unsigned offset_to_line(const std::vector<unsigned>& line_offsets, unsigned offset) {
    auto it = std::upper_bound(line_offsets.begin(), line_offsets.end(), offset);
    return static_cast<unsigned>(it - line_offsets.begin());
}

static std::string sanitize_filename(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (c == ':' || c == '<' || c == '>' || c == '*' || c == '?' ||
            c == '"' || c == '|' || c == '/' || c == '\\' || c == ' ')
            result += '_';
        else
            result += c;
    }

    // A function's name is not bounded. A conversion operator carries its whole target type --
    // `operator typename base_type::value_type()` sanitises to well over three hundred
    // characters, and Boost.Atomic has worse -- which overruns NAME_MAX and makes every later
    // filesystem call on that path throw. The piece's filename already carries a counter that
    // makes it unique, so the readable part can be cut without ambiguity.
    static const size_t max_component = 96;
    if (result.size() > max_component) result.resize(max_component);
    return result;
}

// Functions defined in a translation unit, bucketed by the file that defines them. A
// header's inventory is taken from the translation unit that includes it, in the macro and
// inclusion context its includer establishes, rather than from a standalone re-parse.
// A variable definition written at namespace scope, or an out-of-class static data member.
//
// The preamble is the source with the function bodies carved out, so everything that is not
// a function is copied through verbatim -- and every split piece includes the preamble. An
// ordinary variable with external linkage is therefore defined once per piece, and `ld -r`
// rejects the result. This is the same question has_vague_linkage() answers for functions,
// asked about the other kind of definition a file contains.
struct VariableInfo {
    std::string file;
    std::string name;
    std::string text;                 // source text, through the terminating `;`
    unsigned start_offset = 0;
    unsigned end_offset = 0;
    unsigned start_line = 0;
    std::string type_spelling;        // for the declaration left behind, when the text cannot give one
    unsigned name_offset = 0;         // where the declarator's name is written
    std::vector<ScopeEntry> scope_chain;
    bool is_member = false;           // out-of-class static data member: the class declares it
    bool move_out = false;            // may exist in only one object
    // Kept where it is and marked `inline`, rather than moved to the definitions header.
    // Only from C++17, where inline variables exist.
    bool inline_in_place = false;
    bool internal_linkage = false;    // `static`, or enclosed by an unnamed namespace
    // const or constexpr, asked of the type rather than read off the text. The keyword is
    // routinely a macro -- Boost.Filesystem writes BOOST_CONSTEXPR_OR_CONST -- and such a
    // variable has to stay where its users can see it as a constant expression.
    bool is_const = false;
    bool in_unnamed_ns = false;
    // Moved out and renamed, the way a `static` function in a .cpp already is. Only for the
    // translation unit's own source: see TODO 26.
    bool rename_and_move = false;
    std::string replacement;          // what the preamble gets in place of the definition
};

using HarvestMap = std::map<std::string, std::vector<FunctionInfo>>;
using VarHarvestMap = std::map<std::string, std::vector<VariableInfo>>;

// The functions this translation unit will actually emit.
//
// A header declares far more than any one translation unit uses, and a definition nobody
// uses is never emitted -- that is what `inline` is for. Splitting forces each definition
// into an object of its own with __attribute__((used)), which breaks that: a body the build
// never wanted becomes real code, and its calls to things this translation unit does not
// define turn into references nothing resolves. Boost.Filesystem hides a block of inline
// forwarders behind `#if !defined(BOOST_FILESYSTEM_SOURCE)` so the library never compiles
// them, and from inside the library they look exactly like ordinary functions defined in
// another object -- there is no property of the declaration to test for.
//
// Counting every reference in the translation unit is not enough: a reference inside a body
// that is itself never emitted does not make its target needed. What is needed is what the
// compiler would emit -- start from the definitions it has to emit whatever else happens,
// and follow calls from there.
//
// The two ways of getting this wrong are not symmetric. Missing a root leaves a function in
// the header that could have been split: less splitting, nothing broken. Treating an
// unneeded function as needed is what produces the dangling references above. So the roots
// are kept deliberately narrow.
struct EmitGraph {
    std::string main_file;
    std::map<std::string, std::set<std::string>> calls;  // definition -> what it refers to
    std::set<std::string> roots;
    std::vector<std::string> stack;                      // enclosing definitions
};

static CXChildVisitResult record_reference(CXCursor node, CXCursor, CXClientData payload) {
    auto* g = static_cast<EmitGraph*>(payload);
    const CXCursorKind k = clang_getCursorKind(node);
    if (k == CXCursor_CallExpr || k == CXCursor_DeclRefExpr || k == CXCursor_MemberRefExpr) {
        CXCursor ref = clang_getCursorReferenced(node);
        if (!clang_Cursor_isNull(ref)) {
            std::string target = cx_to_string(clang_getCursorUSR(ref));
            if (!target.empty() && !g->stack.empty())
                g->calls[g->stack.back()].insert(target);
        }
    }
    return CXChildVisit_Recurse;
}

static CXChildVisitResult build_emit_graph(CXCursor node, CXCursor, CXClientData payload) {
    auto* g = static_cast<EmitGraph*>(payload);
    const CXCursorKind k = clang_getCursorKind(node);

    const bool is_function =
        k == CXCursor_FunctionDecl || k == CXCursor_CXXMethod ||
        k == CXCursor_Constructor || k == CXCursor_Destructor ||
        k == CXCursor_FunctionTemplate || k == CXCursor_ConversionFunction;

    if (is_function && clang_isCursorDefinition(node)) {
        const std::string usr = cx_to_string(clang_getCursorUSR(node));
        if (usr.empty()) return CXChildVisit_Continue;

        CXFile file = nullptr;
        clang_getFileLocation(clang_getCursorLocation(node), &file, nullptr, nullptr, nullptr);
        std::error_code ec;
        std::string path;
        if (file)
            path = fs::absolute(cx_to_string(clang_getFileName(file)), ec).lexically_normal().string();

        // A definition the compiler must emit whatever uses it: external linkage and not
        // inline. Which file it was written in does not matter -- a translation unit whose
        // whole body arrives through an implementation include has no definitions of its
        // own, and restricting the roots to the file being compiled left it with no roots
        // at all, so nothing was ever reachable and nothing was ever split.
        (void)g->main_file;
        if (!ec && !path.empty() &&
            !clang_Cursor_isFunctionInlined(node) &&
            clang_getCursorLinkage(node) == CXLinkage_External)
            g->roots.insert(usr);

        g->stack.push_back(usr);
        clang_visitChildren(node, record_reference, g);
        g->stack.pop_back();
        return CXChildVisit_Continue;
    }

    // A namespace-scope variable is initialised whether or not anything reads it, so
    // whatever its initialiser refers to is emitted too.
    if (k == CXCursor_VarDecl && clang_isCursorDefinition(node)) {
        g->stack.push_back("@dynamic-init");
        g->roots.insert("@dynamic-init");
        clang_visitChildren(node, record_reference, g);
        g->stack.pop_back();
        return CXChildVisit_Continue;
    }

    return CXChildVisit_Recurse;
}

static void collect_emitted(CXTranslationUnit tu,
                            const std::string& main_file,
                            std::set<std::string>& out) {
    EmitGraph graph;
    graph.main_file = fs::path(main_file).lexically_normal().string();
    clang_visitChildren(clang_getTranslationUnitCursor(tu), build_emit_graph, &graph);

    // Everything reachable from a root is emitted; nothing else is.
    out = graph.roots;
    std::vector<std::string> work(graph.roots.begin(), graph.roots.end());
    while (!work.empty()) {
        const std::string current = work.back();
        work.pop_back();
        auto it = graph.calls.find(current);
        if (it == graph.calls.end()) continue;
        for (const auto& target : it->second)
            if (out.insert(target).second)
                work.push_back(target);
    }
}

struct VisitorData {
    CXTranslationUnit tu;
    const std::set<std::string>* wanted;   // files whose functions to record
    HarvestMap* harvest;
    VarHarvestMap* variables;
};

static std::string blank_code_noise(const std::string& text);
static bool contains_decl_token(const std::string& blanked, const std::string& kw);

// The source text of a variable definition, through its terminating semicolon.
//
// libclang's extent for a VarDecl stops at the initialiser, so the `;` has to be found in
// the source: without it the definition written into the definitions header does not parse,
// and the `;` left behind in the preamble dangles after the declaration that replaced it.
//
// Extending to the `;` also merges the declarators of `int a = 0, b = 1;` into one range,
// which generate_preamble() then folds and keeps verbatim. That is the right answer -- one
// declarator of such a declaration cannot be moved without the others -- and it falls out of
// the same mechanism that handles macro expansions.
static bool extend_through_semicolon(const std::string& blanked, unsigned& to) {
    int depth = 0;
    for (size_t i = to; i < blanked.size(); ++i) {
        const char c = blanked[i];
        if (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') --depth;
        else if (c == ';' && depth <= 0) { to = static_cast<unsigned>(i) + 1; return true; }
        else if (depth < 0) break;   // ran out of the declaration's scope
    }
    return false;
}

static void harvest_variable(VisitorData* vd, CXCursor cursor, const std::string& file) {
    if (!vd->variables) return;

    CXCursor parent = clang_getCursorSemanticParent(cursor);
    const CXCursorKind pk = clang_getCursorKind(parent);
    const bool at_namespace_scope =
        pk == CXCursor_Namespace || pk == CXCursor_TranslationUnit;
    const bool is_member = pk == CXCursor_ClassDecl || pk == CXCursor_StructDecl ||
                           pk == CXCursor_UnionDecl;
    if (!at_namespace_scope && !is_member) return;

    // A member's *definition* is the one written outside the class; the in-class
    // declaration is not a definition and never reaches here, except for the inline and
    // constexpr cases, which are excluded below anyway.
    if (is_member) {
        const CXCursorKind lex = clang_getCursorKind(clang_getCursorLexicalParent(cursor));
        if (lex == CXCursor_ClassDecl || lex == CXCursor_StructDecl ||
            lex == CXCursor_UnionDecl || lex == CXCursor_ClassTemplate)
            return;
    }

    VariableInfo info;
    info.file = file;
    info.name = cx_to_string(clang_getCursorSpelling(cursor));
    info.is_member = is_member;

    CXSourceRange extent = clang_getCursorExtent(cursor);
    unsigned s_off = 0, e_off = 0, line = 0;
    clang_getFileLocation(clang_getRangeStart(extent), nullptr, &line, nullptr, &s_off);
    clang_getFileLocation(clang_getRangeEnd(extent), nullptr, nullptr, nullptr, &e_off);
    info.start_offset = s_off;
    info.end_offset = e_off;
    info.start_line = line;
    info.type_spelling = cx_to_string(clang_getTypeSpelling(clang_getCursorType(cursor)));
    unsigned name_off = 0;
    clang_getFileLocation(clang_getCursorLocation(cursor), nullptr, nullptr, nullptr, &name_off);
    info.name_offset = name_off;

    // Enclosing namespaces, so a moved definition can be reopened in its own scope. The
    // walk stops at a class for the same reason it does for functions: what is inside one
    // is not something a definitions header can reopen.
    std::vector<ScopeEntry> scope_parts;
    CXCursor up = parent;
    bool in_template = false;
    while (true) {
        const CXCursorKind uk = clang_getCursorKind(up);
        if (uk != CXCursor_Namespace && uk != CXCursor_ClassDecl &&
            uk != CXCursor_StructDecl && uk != CXCursor_UnionDecl &&
            uk != CXCursor_ClassTemplate &&
            uk != CXCursor_ClassTemplatePartialSpecialization)
            break;
        // A static data member of a class template has vague linkage, and a definitions
        // header cannot reopen a template anyway.
        if (uk == CXCursor_ClassTemplate || uk == CXCursor_ClassTemplatePartialSpecialization)
            in_template = true;
        const std::string uname = cx_to_string(clang_getCursorSpelling(up));
        if (uname.empty()) {
            if (uk == CXCursor_Namespace) info.in_unnamed_ns = true;
        } else {
            scope_parts.push_back(
                {uname, uk == CXCursor_Namespace ? ScopeKind::Namespace : ScopeKind::Class});
        }
        up = clang_getCursorSemanticParent(up);
    }
    std::reverse(scope_parts.begin(), scope_parts.end());
    info.scope_chain = scope_parts;

    // Only an ordinary external-linkage definition has to be moved. Everything with vague
    // or internal linkage may appear in every piece, which is what the preamble does.
    // `inline` is read from the pretty-printed declaration rather than the source text
    // because, like constexpr on a function, it is routinely spelled as a macro.
    info.internal_linkage = clang_getCursorLinkage(cursor) != CXLinkage_External;
    info.is_const =
        clang_isConstQualifiedType(clang_getCanonicalType(clang_getCursorType(cursor))) != 0;
    info.move_out = clang_getCursorLinkage(cursor) == CXLinkage_External && !in_template;
    if (info.move_out) {
        const std::string pretty = cx_to_string(clang_getCursorPrettyPrinted(cursor, nullptr));
        const std::string blanked = blank_code_noise(pretty);
        const size_t eq = blanked.find('=');
        const std::string head = blanked.substr(0, eq == std::string::npos ? blanked.size() : eq);
        if (contains_decl_token(head, "inline") || contains_decl_token(head, "constexpr"))
            info.move_out = false;
    }

    (*vd->variables)[file].push_back(std::move(info));
}

// Whether a type stops a function from being defined in another translation unit.
//
// A type declared in an unnamed namespace, or a local class, has no linkage of its own -- or,
// in clang's terms, unique-external linkage. A function whose signature mentions one can only
// be defined in the translation unit that declares the type. Boost.Test writes exactly that:
//
//     namespace { struct unit_test_log_data_helper_impl { ... }; }
//     bool log_entry_start( unit_test_log_data_helper_impl& current_logger_data ) { ... }
//
// and moving the definition into a piece of its own produces
//
//     error: function 'boost::unit_test::log_entry_start' is used but not defined in this
//            translation unit, and cannot be defined in any other translation unit because
//            its type does not have linkage
//
// Every Boost.Geometry test includes the framework in header-only mode, so every one of them
// hit it. Note that the function's own linkage says nothing here: libclang reports
// log_entry_start as external, because it is the *type* that is unique to the unit.
static bool type_lacks_linkage(CXType type, int depth) {
    if (depth > 4) return false;                 // template arguments can nest arbitrarily
    CXType t = clang_getCanonicalType(type);

    // Strip pointers, references and arrays until a declared type is left.
    for (;;) {
        CXType pointee = clang_getPointeeType(t);
        if (pointee.kind != CXType_Invalid) { t = clang_getCanonicalType(pointee); continue; }
        CXType element = clang_getArrayElementType(t);
        if (element.kind != CXType_Invalid) { t = clang_getCanonicalType(element); continue; }
        break;
    }

    CXCursor decl = clang_getTypeDeclaration(t);
    if (!clang_Cursor_isNull(decl)) {
        switch (clang_getCursorLinkage(decl)) {
            case CXLinkage_NoLinkage:
            case CXLinkage_Internal:
            case CXLinkage_UniqueExternal:
                return true;
            default:
                break;
        }
    }

    // The offending type can be a template argument rather than the type itself:
    // std::vector<T> has external linkage while T does not.
    int args = clang_Type_getNumTemplateArguments(t);
    if (args > 16) args = 16;                    // bounded: these lists can be enormous
    for (int i = 0; i < args; ++i) {
        CXType arg = clang_Type_getTemplateArgumentAsType(t, i);
        if (arg.kind != CXType_Invalid && type_lacks_linkage(arg, depth + 1)) return true;
    }
    return false;
}

static CXChildVisitResult visitor(CXCursor cursor, CXCursor /*parent*/, CXClientData data) {
    auto* vd = static_cast<VisitorData*>(data);

    CXSourceLocation loc = clang_getCursorLocation(cursor);
    if (clang_Location_isInSystemHeader(loc))
        return CXChildVisit_Continue;

    CXFile cursor_file;
    clang_getFileLocation(loc, &cursor_file, nullptr, nullptr, nullptr);
    if (!cursor_file)
        return CXChildVisit_Continue;

    std::error_code fec;
    std::string cursor_filename =
        fs::absolute(cx_to_string(clang_getFileName(cursor_file)), fec).lexically_normal().string();
    const bool wanted_file = !fec && vd->wanted->count(cursor_filename) != 0;

    CXCursorKind kind = clang_getCursorKind(cursor);

    bool is_function_def = false;
    // ON, on this branch. See example/conversion-operator/ and TODO/25 defect 3.
    //
    // The paragraph below is what main says, kept so the two can be compared:
    // CXCursor_ConversionFunction is deliberately absent, and its absence is a known hole
    // rather than an oversight: an unharvested definition is never moved out of the preamble,
    // so `context_frame::operator bool()` in Boost.Test's test_tools.ipp -- an out-of-line
    // member with external linkage -- is copied into every piece and `ld -r` rejects the
    // copies. collect_emitted() has always counted conversion functions; only the harvest
    // does not.
    //
    // Adding it here fixes that link failure and produces a program that compiles, links, and
    // then fails at run time: Boost.Geometry's area test reports "no argument provided for
    // parameter color_output" where the plain build passes. Keeping the definitions in the
    // header rather than emitting pieces for them does not help, so it is the *relocation*
    // into the definitions header that is wrong, not the piece. The cause is not understood,
    // and a harvest that changes what a program does is worse than one with a hole in it.
    // See TODO/25.
    if (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod ||
        kind == CXCursor_Constructor || kind == CXCursor_Destructor ||
        kind == CXCursor_ConversionFunction || kind == CXCursor_FunctionTemplate) {
        is_function_def = clang_isCursorDefinition(cursor);
    }

    if (kind == CXCursor_VarDecl && clang_isCursorDefinition(cursor)) {
        if (wanted_file) harvest_variable(vd, cursor, cursor_filename);
        return CXChildVisit_Continue;
    }

    if (!is_function_def) {
        // Recurse regardless of file: a namespace opened in one header can hold definitions
        // belonging to another, and the enclosing scopes are what give a definition its
        // qualification.
        if (kind == CXCursor_Namespace || kind == CXCursor_ClassDecl ||
            kind == CXCursor_StructDecl || kind == CXCursor_ClassTemplate) {
            return CXChildVisit_Recurse;
        }
        return CXChildVisit_Continue;
    }

    if (const char* dump = std::getenv("CPP_SPLITTER_DUMP_HARVEST")) {
        if (cursor_filename.find(dump) != std::string::npos) {
            unsigned line = 0;
            clang_getFileLocation(loc, nullptr, &line, nullptr, nullptr);
            fprintf(stderr, "[harvest] line=%-5u wanted=%d kind=%-3d %s\n", line,
                    (int)wanted_file, (int)kind,
                    cx_to_string(clang_getCursorSpelling(cursor)).c_str());
            fflush(stderr);
        }
    }

    if (!wanted_file)
        return CXChildVisit_Continue;

    FunctionInfo info;
    info.file = cursor_filename;
    info.usr = cx_to_string(clang_getCursorUSR(cursor));
    info.name = cx_to_string(clang_getCursorSpelling(cursor));
    info.is_template = (kind == CXCursor_FunctionTemplate);
    info.is_ctor_or_dtor = (kind == CXCursor_Constructor || kind == CXCursor_Destructor);
    info.is_conversion = (kind == CXCursor_ConversionFunction);
    info.is_inlined = clang_Cursor_isFunctionInlined(cursor) != 0;
    info.external_linkage = (clang_getCursorLinkage(cursor) == CXLinkage_External);
    info.is_static = (clang_getCursorLinkage(cursor) == CXLinkage_Internal);

    // A definition already written out-of-line (`void C::f() { ... }`) needs no rewriting
    // and no declaration left behind: the class already declares it. Only definitions
    // lexically inside the class body do.
    CXCursorKind lex_kind = clang_getCursorKind(clang_getCursorLexicalParent(cursor));
    info.defined_in_class = (lex_kind == CXCursor_ClassDecl ||
                             lex_kind == CXCursor_StructDecl ||
                             lex_kind == CXCursor_UnionDecl ||
                             lex_kind == CXCursor_ClassTemplate ||
                             lex_kind == CXCursor_ClassTemplatePartialSpecialization);

    // `override` and `final` are legal only on the in-class declaration and have to come
    // off an out-of-line definition -- but they are routinely spelled as macros
    // (BOOST_OVERRIDE), which no amount of text matching will find. Rather than mangle the
    // declarator, leave virtual members in the header: they are a small set, and they are
    // the ones where moving a definition is most delicate.
    info.is_virtual = clang_CXXMethod_isVirtual(cursor) != 0;

    // Deliberately not evaluated here. Walking canonical types and their template arguments
    // is expensive -- on Boost.Geometry, doing it for every harvested function took a
    // translation unit from about a minute to over half an hour -- and almost every function
    // is kept for some cheaper reason first. prepare_functions() asks only about the ones
    // that would otherwise be split.
    info.fn_type = clang_getCursorType(cursor);

    // An explicit specialization is introduced by a `template< >` prefix that sits outside
    // the cursor's extent. Moving the definition out would strand that prefix in the
    // header, so these stay put.
    info.is_specialization =
        !clang_Cursor_isNull(clang_getSpecializedCursorTemplate(cursor));

    // Whether the definition is constexpr cannot be read off the source text: the keyword
    // is routinely hidden behind a macro (BOOST_SYSTEM_CONSTEXPR and friends). The
    // pretty-printed declaration has the specifiers resolved, so ask for that instead.
    // Moving a constexpr body out of the header is not allowed -- callers need it to
    // constant-initialise, and `static constexpr T instance{};` stops compiling without it.
    {
        std::string pretty = cx_to_string(clang_getCursorPrettyPrinted(cursor, nullptr));
        const std::string blanked = blank_code_noise(pretty);
        const size_t body = blanked.find('{');
        const std::string head = blanked.substr(0, body == std::string::npos ? blanked.size() : body);
        info.is_constexpr = contains_decl_token(head, "constexpr") ||
                            contains_decl_token(head, "consteval");

        if (pretty.find("always_inline") != std::string::npos) {
            clang_visitChildren(cursor,
                [](CXCursor attr, CXCursor, CXClientData payload) -> CXChildVisitResult {
                    if (!clang_isAttribute(clang_getCursorKind(attr)))
                        return CXChildVisit_Continue;
                    CXSourceRange r = clang_getCursorExtent(attr);
                    unsigned begin = 0, end = 0;
                    clang_getFileLocation(clang_getRangeStart(r), nullptr, nullptr, nullptr, &begin);
                    clang_getFileLocation(clang_getRangeEnd(r), nullptr, nullptr, nullptr, &end);
                    if (end > begin)
                        static_cast<FunctionInfo*>(payload)->always_inline_ranges.emplace_back(begin, end);
                    return CXChildVisit_Continue;
                },
                &info);
        }
    }

    CXString qualified = clang_getCursorDisplayName(cursor);
    info.qualified_name = cx_to_string(qualified);

    CXType ret_type = clang_getCursorResultType(cursor);
    info.return_type = cx_to_string(clang_getTypeSpelling(ret_type));

    std::string class_prefix;
    CXCursor parent_cursor = clang_getCursorSemanticParent(cursor);
    std::vector<ScopeEntry> scope_parts;
    bool innermost = true;   // still counting unnamed namespaces closest to the function
    while (true) {
        CXCursorKind pk = clang_getCursorKind(parent_cursor);
        if (pk == CXCursor_ClassDecl || pk == CXCursor_StructDecl ||
            pk == CXCursor_UnionDecl || pk == CXCursor_ClassTemplate ||
            pk == CXCursor_ClassTemplatePartialSpecialization ||
            pk == CXCursor_Namespace) {
            std::string pname = cx_to_string(clang_getCursorSpelling(parent_cursor));
            ScopeKind sk = (pk == CXCursor_Namespace) ? ScopeKind::Namespace : ScopeKind::Class;
            if (sk == ScopeKind::Class) {
                // An out-of-line definition of a member of a class template would have to
                // repeat the template header, which a split .cpp cannot do; an unnamed
                // class offers no name to qualify the definition with.
                if (pk == CXCursor_ClassTemplate ||
                    pk == CXCursor_ClassTemplatePartialSpecialization)
                    info.in_class_template = true;
                if (pname.empty())
                    info.in_anonymous_class = true;

                // A full explicit specialization is reported as a plain StructDecl, and its
                // spelling is the template's name with the arguments dropped. Qualifying a
                // definition with that gives `builtin_clz_dispatch::call` for a member of
                // `builtin_clz_dispatch<unsigned long>`, which names nothing. Unlike a class
                // template, a full specialization *can* have its members defined out of line
                // with no template header, so the definition is movable -- only the name was
                // wrong. The display name carries the arguments.
                if (!clang_Cursor_isNull(clang_getSpecializedCursorTemplate(parent_cursor))) {
                    const std::string display =
                        cx_to_string(clang_getCursorDisplayName(parent_cursor));
                    if (display.find('<') != std::string::npos &&
                        display.find("(anonymous") == std::string::npos &&
                        display.find("(lambda") == std::string::npos) {
                        pname = display;
                    } else {
                        // A name that cannot be written cannot qualify anything.
                        info.in_specialization_without_name = true;
                    }
                }
            }
            if (pname.empty()) {
                // An unnamed namespace contributes no name to qualify with, but the split
                // definition is hoisted out of it, so the count is needed to place the
                // declaration in the same scope as the definition. Anything enclosed by one
                // anywhere up the chain has internal linkage -- a member of a named class
                // inside an unnamed namespace just as much as a free function in it -- so
                // count it wherever it appears, not only closest in.
                if (sk == ScopeKind::Namespace) {
                    info.in_unnamed_ns = true;
                    if (innermost) ++info.unnamed_ns_depth;
                }
            } else {
                innermost = false;
                scope_parts.push_back({pname, sk});
            }
            parent_cursor = clang_getCursorSemanticParent(parent_cursor);
        } else {
            break;
        }
    }
    std::reverse(scope_parts.begin(), scope_parts.end());
    info.scope_chain = scope_parts;

    for (const auto& part : scope_parts)
        class_prefix += part.name + "::";

    std::string full_sig;
    if (kind == CXCursor_Constructor) {
        full_sig = class_prefix + info.qualified_name;
    } else if (kind == CXCursor_Destructor) {
        full_sig = class_prefix + info.qualified_name;
    } else {
        full_sig = info.return_type + " " + class_prefix + info.qualified_name;
    }
    info.signature = full_sig;

    CXSourceRange extent = clang_getCursorExtent(cursor);
    CXSourceLocation start_loc = clang_getRangeStart(extent);
    CXSourceLocation end_loc = clang_getRangeEnd(extent);

    unsigned start_line, end_line, s_off, e_off;
    clang_getFileLocation(start_loc, nullptr, &start_line, nullptr, &s_off);
    clang_getFileLocation(end_loc, nullptr, &end_line, nullptr, &e_off);
    info.start_line = start_line;
    info.end_line = end_line;
    info.start_offset = s_off;
    info.end_offset = e_off;

    info.body = get_source_range_text(vd->tu, extent);

    (*vd->harvest)[cursor_filename].push_back(std::move(info));

    return CXChildVisit_Continue;
}

// Decided by prepare_functions(), which needs the source text and so cannot run here.
// Why a definition was left in the preamble. The piece is still written, so that the file
// list stays stable, but it is not compiled -- and the reader of that file deserves to know
// which of the many keep rules applied rather than being told every one of them is
// "template", which is what this used to say.
// A definition that may appear in every object that includes it: inline, a template, or
// anything without external linkage. Only these are safe to leave in the shared preamble.
static bool has_vague_linkage(const FunctionInfo& fn) {
    return fn.is_inlined || fn.is_template || fn.in_class_template || !fn.external_linkage;
}

static std::string keep_reason(const FunctionInfo& fn) {
    if (fn.shares_extent)       return "shares its source extent with another definition";
    if (fn.macro_invocation)    return "produced by a macro invocation, which moved as a unit";
    if (fn.uses_undefined_macro) return "needs a macro the file undefines";
    if (fn.is_template)         return "function template";
    if (fn.in_class_template)   return "member of a class template";
    if (fn.in_anonymous_class)  return "member of an unnamed class";
    if (fn.signature_lacks_linkage)
        return "a type in its signature has no linkage";
    if (fn.in_specialization_without_name)
        return "member of a specialization whose name cannot be written";
    if (fn.is_specialization)   return "explicit specialization";
    if (fn.is_virtual)          return "virtual member function";
    if (fn.in_unnamed_ns)       return "enclosed by an unnamed namespace";
    if (fn.is_static)           return "internal linkage in a header";
    if (fn.name == "main" && fn.scope_chain.empty()) return "the program's entry point";
    if (fn.is_ctor_or_dtor)     return "constructor or destructor in a header";
    if (fn.is_conversion)       return "conversion operator: no return type to rebuild it from";
    if (!fn.always_inline_ranges.empty()) return "always-inline";
    return "not emitted by this translation unit, or not movable out of the header";
}

static bool should_keep_in_header(const FunctionInfo& fn) {
    return fn.keep_in_header;
}

static std::string make_static_mangled_name(const std::string& stem, const std::string& name) {
    std::string safe_stem;
    for (char c : stem) {
        safe_stem += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return "__static_" + safe_stem + "__" + name;
}

// --- Lightweight C++ lexical helpers -------------------------------------------------
//
// Text-level edits on extracted source (stripping declaration specifiers, renaming
// identifiers) must only ever match whole identifier tokens, and must never fire inside
// a string literal, a character literal or a comment. blank_code_noise() returns a copy
// of its input in which all of those have been overwritten with spaces of the same
// length, so offsets in the copy map 1:1 onto the original: searches run on the copy,
// edits apply to the original. Newlines are preserved so line numbering survives.

static bool is_ident_char(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_';
}

static std::string blank_code_noise(const std::string& text) {
    std::string out = text;
    const size_t n = text.size();

    auto blank = [&](size_t from, size_t to) {
        for (size_t k = from; k < to && k < n; ++k)
            if (out[k] != '\n') out[k] = ' ';
    };

    size_t i = 0;
    while (i < n) {
        if (text[i] == '/' && i + 1 < n && text[i + 1] == '/') {
            size_t j = i;
            while (j < n && text[j] != '\n') ++j;
            blank(i, j);
            i = j;
            continue;
        }
        if (text[i] == '/' && i + 1 < n && text[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(text[j] == '*' && text[j + 1] == '/')) ++j;
            j = (j + 1 < n) ? j + 2 : n;
            blank(i, j);
            i = j;
            continue;
        }

        // Raw string literal: R"delim( ... )delim" (any encoding prefix precedes the R).
        if (text[i] == 'R' && i + 1 < n && text[i + 1] == '"') {
            size_t delim_begin = i + 2;
            size_t delim_end = text.find('(', delim_begin);
            if (delim_end != std::string::npos) {
                std::string closer = ")" + text.substr(delim_begin, delim_end - delim_begin) + "\"";
                size_t close = text.find(closer, delim_end + 1);
                size_t j = (close == std::string::npos) ? n : close + closer.size();
                blank(i, j);
                i = j;
                continue;
            }
        }

        if (text[i] == '\'') {
            // A quote directly following an identifier token is a digit separator
            // (1'000'000), not the start of a character literal -- unless that token is
            // one of the character-literal encoding prefixes.
            size_t tok_end = i, tok_begin = i;
            while (tok_begin > 0 && is_ident_char(static_cast<unsigned char>(text[tok_begin - 1])))
                --tok_begin;
            std::string prev = text.substr(tok_begin, tok_end - tok_begin);
            if (!prev.empty() && prev != "L" && prev != "u" && prev != "U" && prev != "u8") {
                ++i;
                continue;
            }
        }

        if (text[i] == '"' || text[i] == '\'') {
            char quote = text[i];
            size_t j = i + 1;
            while (j < n) {
                if (text[j] == '\\') { j += 2; continue; }
                if (text[j] == quote) { ++j; break; }
                if (text[j] == '\n') break;   // unterminated: give up at end of line
                ++j;
            }
            blank(i, j);
            i = j;
            continue;
        }

        ++i;
    }
    return out;
}

// End of the declaration prefix of a function definition: everything before the
// parameter list, or failing that before the body. Declaration specifiers can only
// appear in this region, so bounding a search to it is what keeps a search for `static`
// away from a `static_cast` in the body.
static size_t decl_prefix_end(const std::string& blanked) {
    size_t paren = blanked.find('(');
    size_t brace = blanked.find('{');
    if (paren != std::string::npos && (brace == std::string::npos || paren < brace))
        return paren;
    if (brace != std::string::npos)
        return brace;
    return blanked.size();
}

// Remove a leading declaration specifier (`static`, `inline`, ...) from `text` if it is
// present as a whole token in the declaration prefix, otherwise return `text` unchanged.
// Trailing spaces and tabs go with it, but never a newline: split files carry a #line
// directive and the body's line numbering has to survive.
static std::string strip_decl_specifier(const std::string& text,
                                        const std::string& keyword) {
    const std::string blanked = blank_code_noise(text);
    const size_t limit = decl_prefix_end(blanked);

    size_t pos = 0;
    while ((pos = blanked.find(keyword, pos)) != std::string::npos && pos < limit) {
        const size_t end = pos + keyword.size();
        const bool left_ok = (pos == 0) ||
            !is_ident_char(static_cast<unsigned char>(blanked[pos - 1]));
        const bool right_ok = (end >= blanked.size()) ||
            !is_ident_char(static_cast<unsigned char>(blanked[end]));
        if (left_ok && right_ok) {
            size_t after = end;
            while (after < text.size() && (text[after] == ' ' || text[after] == '\t'))
                ++after;
            std::string result = text;
            result.erase(pos, after - pos);
            return result;
        }
        pos = end;
    }
    return text;
}


static std::string extract_source_signature(const std::string& source,
                                              const FunctionInfo& fn) {
    std::string body_text = source.substr(fn.start_offset,
                                           fn.end_offset - fn.start_offset);
    int brace_depth = 0;
    size_t body_start = std::string::npos;
    for (size_t i = 0; i < body_text.size(); ++i) {
        if (body_text[i] == '{') {
            if (brace_depth == 0) {
                body_start = i;
                break;
            }
            ++brace_depth;
        } else if (body_text[i] == '}') {
            --brace_depth;
        }
    }
    if (body_start == std::string::npos)
        return "";

    std::string sig = body_text.substr(0, body_start);
    while (!sig.empty() && (sig.back() == ' ' || sig.back() == '\n' ||
                            sig.back() == '\r' || sig.back() == '\t'))
        sig.pop_back();
    return sig;
}

static size_t definition_decl_end(const std::string& blanked);
static size_t find_declarator(const std::string& blanked, const std::string& name);
static std::string trim_ws(const std::string& s);
static bool token_at(const std::string& blanked, size_t pos, size_t len);

// Terminate a declaration built from source text.
//
// The text runs up to the body's opening brace, and what precedes that brace may be a
// trailing `//` comment -- Boost.Chrono writes a two-line comment between `tick_factor()`
// and its `{`. A semicolon appended to such text lands inside the comment, and the
// declaration is left with no terminator at all. Putting it on a line of its own costs a
// newline and is always correct.
static std::string terminate_declaration(const std::string& decl) {
    const std::string blanked = blank_code_noise(decl);
    const size_t nl = decl.rfind('\n');
    const size_t from = (nl == std::string::npos) ? 0 : nl + 1;
    // blank_code_noise() preserves offsets and newlines, so the two last lines line up. If
    // they differ the line ends inside a comment that nothing closed.
    if (decl.compare(from, std::string::npos, blanked, from, std::string::npos) != 0)
        return decl + "\n;";
    return decl + ";";
}

// Declaration left in place of a removed free-function definition. It is emitted at the
// position the definition occupied, so it inherits the surrounding namespaces and any #if
// context and needs no wrapping of its own. Emitting it here rather than appending it to
// the end of the preamble also means it precedes every use the original file had: a
// function pointer initialised at namespace scope just below the definition would
// otherwise refer to a name that has not been declared yet.
static std::string generate_forward_decl_inplace(const FunctionInfo& fn,
                                                 const std::string& stem) {
    // Members keep their declaration inside the class body instead -- except an explicit
    // specialization, which the class body does not declare: what is written there is the
    // primary template's member. A specialization must be declared before the first use
    // that would instantiate it, and moving its definition into the definitions header
    // puts it after every such use, so the declaration has to stay behind in its place.
    if (!fn.is_specialization)
        for (const auto& entry : fn.scope_chain)
            if (entry.kind == ScopeKind::Class)
                return "";

    const size_t decl_end = definition_decl_end(blank_code_noise(fn.body));
    if (decl_end == std::string::npos)
        return "";

    std::string sig = trim_ws(fn.body.substr(0, decl_end));
    if (sig.empty())
        return "";

    sig = strip_decl_specifier(sig, "inline");
    if (fn.is_static) {
        sig = strip_decl_specifier(sig, "static");
        const size_t name_pos = find_declarator(blank_code_noise(sig), fn.name);
        if (name_pos == std::string::npos)
            return "";
        sig.replace(name_pos, fn.name.size(), make_static_mangled_name(stem, fn.name));
    }
    return terminate_declaration(sig);
}

// Declaration appended at the end of the preamble, wrapped in its namespaces. Used for
// every function whose name did not change: their uses all live inside other function
// bodies, which are themselves moved into split files that include the whole preamble, so
// the position of the declaration does not matter. Emitting these in place instead would
// mean rewriting macro-expanded and preprocessed headers, where a function's source extent
// is the macro invocation rather than a declarator.
static std::string generate_forward_decl_wrapped(const FunctionInfo& fn,
                                          const std::string& stem,
                                          const std::string& source) {
    bool is_class_method = false;
    for (const auto& entry : fn.scope_chain) {
        if (entry.kind == ScopeKind::Class) {
            is_class_method = true;
            break;
        }
    }
    // Members need no trailing declaration: prepare_functions() leaves one inside the
    // class body, which is the only place a member can be declared.
    if (is_class_method)
        return "";

    std::string sig = extract_source_signature(source, fn);
    if (sig.empty())
        return "";

    sig = strip_decl_specifier(sig, "inline");

    if (fn.is_static) {
        sig = strip_decl_specifier(sig, "static");
        size_t npos = sig.find(fn.name + "(");
        if (npos == std::string::npos)
            npos = sig.find(fn.name);
        if (npos != std::string::npos) {
            sig.replace(npos, fn.name.size(),
                        make_static_mangled_name(stem, fn.name));
        }
    }

    std::vector<std::string> ns_names;
    for (const auto& entry : fn.scope_chain) {
        if (entry.kind == ScopeKind::Namespace)
            ns_names.push_back(entry.name);
    }

    if (!ns_names.empty()) {
        std::string ns_prefix;
        for (const auto& ns : ns_names)
            ns_prefix += ns + "::";
        size_t fname_pos = sig.find(ns_prefix + fn.name);
        if (fname_pos != std::string::npos) {
            sig.erase(fname_pos, ns_prefix.size());
        }
    }

    std::string decl;
    for (const auto& ns : ns_names)
        decl += "namespace " + ns + " { ";
    decl += terminate_declaration(sig);
    for (size_t i = 0; i < ns_names.size(); ++i)
        decl += " }";

    return decl;
}

// Re-emit a definition with its always_inline attributes removed, so that the compiler
// produces a real out-of-line body for it instead of an available_externally one that
// nothing can link against. The attribute is usually reached through a macro that also
// supplies `inline` (BOOST_FORCEINLINE and its equivalents), and the whole macro token is
// what the attribute's source range covers, so `inline` is put back when removing it would
// otherwise leave the definition with external linkage and collide across translation
// units.
// True when the definition opens with a template header. `inline` may not precede one, and
// a template already has the vague linkage that adding `inline` would be asking for.
static bool opens_with_template(const std::string& blanked) {
    size_t i = 0;
    while (i < blanked.size() && std::isspace(static_cast<unsigned char>(blanked[i]))) ++i;
    return blanked.compare(i, 8, "template") == 0 &&
           (i + 8 >= blanked.size() || !is_ident_char(static_cast<unsigned char>(blanked[i + 8])));
}

// Where to insert `inline` in a definition that has lost it, or npos when it must not be
// inserted at all.
//
// A template definition needs no `inline` and may not take one in front of its parameter
// list. An explicit specialization is not a template, though: `template <>` introduces a
// plain function, which needs `inline` exactly as much as any other -- and the keyword has to
// go *after* the prefix, because `inline template <>` is not a declaration.
//
// Boost.QVM is where this matters:
//
//     template <> BOOST_QVM_INLINE_TRIVIAL long double floor<long double>( long double )
//
// The macro carries an always-inline attribute and the `inline`, so stripping the attribute
// took both. Judged a template and left alone, what remained was a strong definition in a
// header every piece includes, and `ld -r` rejected the copies.
static size_t inline_insertion_point(const std::string& blanked) {
    size_t i = 0;
    while (i < blanked.size() && std::isspace(static_cast<unsigned char>(blanked[i]))) ++i;
    if (!opens_with_template(blanked)) return 0;

    size_t j = i + 8;
    while (j < blanked.size() && std::isspace(static_cast<unsigned char>(blanked[j]))) ++j;
    if (j >= blanked.size() || blanked[j] != '<') return std::string::npos;

    const size_t open = j;
    int depth = 0;
    for (; j < blanked.size(); ++j) {
        if (blanked[j] == '<') ++depth;
        else if (blanked[j] == '>' && --depth == 0) break;
    }
    if (j >= blanked.size()) return std::string::npos;

    // Only an *empty* parameter list makes this a specialization rather than a template.
    for (size_t k = open + 1; k < j; ++k)
        if (!std::isspace(static_cast<unsigned char>(blanked[k]))) return std::string::npos;

    return j + 1;
}

static std::string strip_always_inline(const std::string& text, unsigned base,
                                       const std::vector<std::pair<unsigned, unsigned>>& ranges) {
    if (ranges.empty()) return text;

    std::vector<std::pair<unsigned, unsigned>> local;
    for (const auto& r : ranges) {
        if (r.first < base) continue;
        size_t from = r.first - base, to = r.second - base;
        if (to > text.size() || from >= to) continue;
        // An attribute that spans the whole definition did not come from the text it would
        // be cut out of. Its tokens arrived through a macro, so libclang reports the macro
        // invocation as the attribute's extent -- and the invocation is also the
        // definition's extent. Erasing it deletes the definition, and every other
        // declaration the same macro produced, leaving a bare `inline` in a class body.
        if (from == 0 && to == text.size()) continue;
        local.emplace_back(from, to);
    }
    if (local.empty()) return text;

    std::sort(local.begin(), local.end(),
              [](const std::pair<unsigned, unsigned>& a, const std::pair<unsigned, unsigned>& b) {
                  return a.first > b.first;
              });

    std::string out = text;
    for (const auto& r : local)
        out.erase(r.first, r.second - r.first);

    const std::string blanked = blank_code_noise(out);
    const size_t at = inline_insertion_point(blanked);
    if (at != std::string::npos &&
        !contains_decl_token(blanked.substr(0, decl_prefix_end(blanked)), "inline"))
        out.insert(at, at == 0 ? "inline " : " inline");
    return out;
}

// --- Renaming of internal-linkage functions ------------------------------------------
//
// Split-out functions with internal linkage are renamed so that separately compiled
// objects can be combined with `ld -r` without symbol collisions. The rename has to reach
// every place the identifier appears -- the split bodies, the declarations, and the text
// carried verbatim into the preamble. Missing that last one breaks any file that installs
// such a function behind a function pointer, which is a common idiom:
//
//     fill_random_t* fill_random = &fill_random_dev_random;   // undeclared after renaming
using StaticRenameMap = std::vector<std::pair<std::string, std::string>>;

static StaticRenameMap build_static_rename_map(const std::vector<FunctionInfo>& functions,
                                               const std::vector<VariableInfo>& variables,
                                               const std::string& unit_tag) {
    StaticRenameMap renames;
    std::set<std::string> seen;

    // Variables that are being moved out of the preamble and renamed. Entering them here is
    // what rewrites every *use* of the name -- in the retained preamble text, in the split
    // bodies, and in the moved definition itself -- which is the part a hand-rolled rename
    // would get wrong.
    for (const auto& var : variables) {
        if (!var.rename_and_move) continue;
        if (seen.insert(var.name).second)
            renames.emplace_back(var.name, make_static_mangled_name(unit_tag, var.name));
    }

    for (const auto& fn : functions) {
        // Only free functions are renamed. A member's name is part of its class, and
        // renaming one breaks every use of it -- an override stops overriding, and callers
        // stop finding it. Members of a class with internal linkage report internal linkage
        // themselves, so `is_static` alone is not the right test.
        bool is_member = false;
        for (const auto& e : fn.scope_chain)
            if (e.kind == ScopeKind::Class) { is_member = true; break; }
        if (is_member) continue;
        if (fn.keep_in_header) continue;   // never moved, so never renamed

        // Overloads share one name and so one mangled name; entering it twice would make
        // the rewriter replace the same position twice and corrupt the identifier.
        if (fn.is_static && seen.insert(fn.name).second)
            renames.emplace_back(fn.name, make_static_mangled_name(unit_tag, fn.name));
    }
    return renames;
}

// Rewrite whole-identifier occurrences of each renamed function. Matching runs over a copy
// with string literals, character literals and comments blanked out, so the name appearing
// in a diagnostic message or a comment is left alone.
static std::string apply_static_renames(const std::string& text,
                                        const StaticRenameMap& renames) {
    if (renames.empty() || text.empty()) return text;

    const std::string blanked = blank_code_noise(text);
    struct Hit { size_t pos; size_t len; const std::string* to; };
    std::vector<Hit> hits;

    for (const auto& entry : renames) {
        const std::string& orig = entry.first;
        if (orig.empty()) continue;
        size_t pos = 0;
        while ((pos = blanked.find(orig, pos)) != std::string::npos) {
            if (token_at(blanked, pos, orig.size()))
                hits.push_back({pos, orig.size(), &entry.second});
            pos += orig.size();
        }
    }
    // Apply back to front so the earlier offsets stay valid as lengths change, and never
    // rewrite the same position twice.
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.pos > b.pos; });
    hits.erase(std::unique(hits.begin(), hits.end(),
                           [](const Hit& a, const Hit& b) { return a.pos == b.pos; }),
               hits.end());

    std::string out = text;
    for (const auto& h : hits)
        out.replace(h.pos, h.len, *h.to);
    return out;
}

// --- Out-of-line rendering of class member functions ---------------------------------
//
// A member function defined inside its class body cannot simply be moved into a .cpp:
// `T name(args) const { ... }` is not a valid non-member definition -- `const` is
// illegal, `this` is unavailable, and a constructor's member-initialiser list parses as a
// base-initialiser on a free function. It has to be rewritten into the out-of-line form
// `T Class::name(args) const { ... }` with the specifiers that are legal only in-class
// removed, and the class has to keep a declaration where the definition used to be.
// Anything that cannot be expressed out-of-line stays in the header instead.

// Offset where the declarator ends: the ':' introducing a constructor's member
// initialiser list, or the '{' opening the body, whichever comes first at top level.
static size_t definition_decl_end(const std::string& blanked) {
    int paren = 0, brack = 0;
    for (size_t i = 0; i < blanked.size(); ++i) {
        char c = blanked[i];
        if (c == '(') ++paren;
        else if (c == ')') --paren;
        else if (c == '[') ++brack;
        else if (c == ']') --brack;
        else if (paren == 0 && brack == 0) {
            if (c == '{') return i;
            if (c == ':') {
                if (i + 1 < blanked.size() && blanked[i + 1] == ':') { ++i; continue; }
                return i;
            }
        }
    }
    return std::string::npos;
}

static bool token_at(const std::string& blanked, size_t pos, size_t len) {
    bool left = pos == 0 || !is_ident_char(static_cast<unsigned char>(blanked[pos - 1]));
    bool right = pos + len >= blanked.size() ||
                 !is_ident_char(static_cast<unsigned char>(blanked[pos + len]));
    return left && right;
}

static bool contains_decl_token(const std::string& blanked, const std::string& kw) {
    size_t pos = 0;
    while ((pos = blanked.find(kw, pos)) != std::string::npos) {
        if (token_at(blanked, pos, kw.size())) return true;
        pos += kw.size();
    }
    return false;
}

// Offset of the declarator name: the first whole-token occurrence of `name` followed by
// an opening parenthesis. Taking the first match rather than the last keeps a parameter
// whose type happens to share the name from winning.
static size_t find_declarator(const std::string& blanked, const std::string& name) {
    if (name.empty()) return std::string::npos;
    size_t pos = 0;
    while ((pos = blanked.find(name, pos)) != std::string::npos) {
        if (token_at(blanked, pos, name.size())) {
            size_t j = pos + name.size();
            while (j < blanked.size() && std::isspace(static_cast<unsigned char>(blanked[j])))
                ++j;
            if (j < blanked.size() && blanked[j] == '(') return pos;
        }
        pos += name.size();
    }
    return std::string::npos;
}

// `override` and `final` are legal only on the in-class declaration. They can appear only
// after the parameter list, so the search starts past the last top-level ')'.
static void strip_virt_specifiers(std::string& decl) {
    std::string b = blank_code_noise(decl);
    int depth = 0;
    size_t after_params = std::string::npos;
    for (size_t i = 0; i < b.size(); ++i) {
        if (b[i] == '(') ++depth;
        else if (b[i] == ')') { if (--depth == 0) after_params = i + 1; }
    }
    if (after_params == std::string::npos) return;

    for (const std::string& kw : {std::string("override"), std::string("final")}) {
        size_t pos = b.find(kw, after_params);
        while (pos != std::string::npos) {
            if (token_at(b, pos, kw.size())) {
                decl.erase(pos, kw.size());
                b = blank_code_noise(decl);
                pos = b.find(kw, after_params);
            } else {
                pos = b.find(kw, pos + kw.size());
            }
        }
    }
}

// Remove default arguments from the parameter list opening at `open`. An out-of-line
// definition may not repeat them. Returns false if the list looks malformed, in which case
// the caller keeps the function in the header rather than guessing.
static bool strip_default_args(std::string& decl, size_t open) {
    std::string b = blank_code_noise(decl);
    if (open >= b.size() || b[open] != '(') return false;

    std::vector<std::pair<size_t, size_t>> cuts;
    int depth = 0, angle = 0;
    size_t eq = std::string::npos;
    bool closed = false;

    for (size_t i = open; i < b.size(); ++i) {
        char c = b[i];
        if (c == '(' || c == '[' || c == '{') { ++depth; continue; }
        if (c == ')' || c == ']' || c == '}') {
            if (--depth == 0) {
                if (eq != std::string::npos) cuts.emplace_back(eq, i);
                closed = true;
                break;
            }
            continue;
        }
        if (depth != 1) continue;
        if (c == '<') { ++angle; continue; }
        if (c == '>') { if (angle > 0) --angle; continue; }
        if (angle != 0) continue;
        if (c == ',') {
            if (eq != std::string::npos) { cuts.emplace_back(eq, i); eq = std::string::npos; }
            continue;
        }
        if (c == '=' && eq == std::string::npos) {
            if (i + 1 < b.size() && b[i + 1] == '=') { ++i; continue; }
            char prev = i > 0 ? b[i - 1] : ' ';
            if (prev == '=' || prev == '!' || prev == '<' || prev == '>' || prev == '+' ||
                prev == '-' || prev == '*' || prev == '/' || prev == '%' || prev == '&' ||
                prev == '|' || prev == '^')
                continue;
            eq = i;
        }
    }
    if (!closed) return false;

    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it)
        decl.erase(it->first, it->second - it->first);
    return true;
}

// Skip a leading run of [[attribute]] groups and whitespace.
static size_t skip_leading_attributes(const std::string& blanked) {
    size_t i = 0;
    while (i < blanked.size()) {
        while (i < blanked.size() && std::isspace(static_cast<unsigned char>(blanked[i]))) ++i;
        if (i + 1 < blanked.size() && blanked[i] == '[' && blanked[i + 1] == '[') {
            int depth = 0;
            size_t j = i;
            for (; j < blanked.size(); ++j) {
                if (blanked[j] == '[') ++depth;
                else if (blanked[j] == ']') { if (--depth == 0) { ++j; break; } }
            }
            i = j;
            continue;
        }
        break;
    }
    return i;
}

// True if the declarator already carries a trailing return type.
static bool has_trailing_return(const std::string& blanked, size_t params_open) {
    int depth = 0;
    size_t after = std::string::npos;
    for (size_t i = params_open; i < blanked.size(); ++i) {
        if (blanked[i] == '(') ++depth;
        else if (blanked[i] == ')') { if (--depth == 0) { after = i + 1; break; } }
    }
    if (after == std::string::npos) return false;
    return blanked.find("->", after) != std::string::npos;
}

static std::string trim_ws(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// The identifier of a `!defined(X)` condition, or empty if the expression is anything else.
// An include guard is as often written `#if !defined(BOOST_FOO_HPP)` as `#ifndef
// BOOST_FOO_HPP`, and the two have to be recognised alike: a guard replayed in a split
// piece is always false, because the header it guards has just been included.
static std::string negated_defined_operand(const std::string& expr) {
    std::string s = trim_ws(expr);
    if (s.empty() || s[0] != '!') return "";
    s = trim_ws(s.substr(1));
    const std::string kw = "defined";
    if (s.compare(0, kw.size(), kw) != 0) return "";
    s = trim_ws(s.substr(kw.size()));
    const bool paren = !s.empty() && s[0] == '(';
    if (paren) s = trim_ws(s.substr(1));
    size_t n = 0;
    while (n < s.size() && is_ident_char(static_cast<unsigned char>(s[n]))) ++n;
    const std::string ident = s.substr(0, n);
    std::string tail = trim_ws(s.substr(n));
    if (paren) {
        if (tail.empty() || tail[0] != ')') return "";
        tail = trim_ws(tail.substr(1));
    }
    // Anything left over makes this a compound condition, not a bare guard.
    return tail.empty() ? ident : std::string();
}

// The source split into *logical* lines: a line ending in a backslash is joined with the
// one after it, which is what the preprocessor sees. A directive scanner that works on
// physical lines stores half of a continued condition, and a `#if A && \` replayed in a
// split piece splices whatever follows it -- the `#line` directive -- into the condition.
//
// Each entry carries the offset the logical line started at, because active_conditionals()
// compares that against a definition's start offset, and getting it wrong changes which
// conditionals a definition is reported to be inside. The backslash and its newline are
// replaced by a single space so that tokens either side of the splice stay separate.
struct LogicalLine {
    std::string text;
    unsigned start;    // offset of the first physical line
    unsigned end;      // offset just past the last physical line's newline
};

static std::vector<LogicalLine> logical_lines(const std::string& source) {
    std::vector<LogicalLine> out;
    const size_t n = source.size();
    size_t i = 0;
    while (i < n) {
        LogicalLine ll;
        ll.start = static_cast<unsigned>(i);
        while (i < n) {
            size_t eol = source.find('\n', i);
            const size_t stop = (eol == std::string::npos) ? n : eol;
            std::string piece = source.substr(i, stop - i);
            i = (eol == std::string::npos) ? n : eol + 1;

            // A splice is a backslash immediately before the newline, ignoring the \r of a
            // CRLF file. Trailing blanks after it are not a splice, and neither compiler
            // treats them as one without a warning.
            if (!piece.empty() && piece.back() == '\r') piece.pop_back();
            const bool spliced = !piece.empty() && piece.back() == '\\';
            if (spliced) {
                piece.pop_back();
                ll.text += piece;
                ll.text += ' ';
                if (i < n) continue;
            } else {
                ll.text += piece;
            }
            break;
        }
        ll.end = static_cast<unsigned>(i);
        out.push_back(std::move(ll));
    }
    return out;
}

// The preprocessor conditionals active at `offset`, outermost first, with the file's own
// include guard left out. A definition written inside `#if X` must be emitted inside the
// same `#if X` in its split file: otherwise it is compiled unconditionally, and a
// definition that the real build never sees -- one referring to a type only available under
// that condition -- breaks the piece.
static std::vector<std::string> active_conditionals(const std::string& source,
                                                    unsigned offset) {
    std::vector<std::string> stack;
    bool guard_skipped = false;
    std::string pending_guard;

    for (const auto& ll : logical_lines(source)) {
        if (ll.start >= offset) break;

        std::string t = trim_ws(ll.text);
        if (t.empty() || t[0] != '#') {
            pending_guard.clear();
            continue;
        }
        std::string rest = trim_ws(t.substr(1));
        std::istringstream ls(rest);
        std::string directive, ident;
        ls >> directive >> ident;

        if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
            std::string guard = (directive == "ifndef")
                                    ? ident
                                    : (directive == "if"
                                           ? negated_defined_operand(trim_ws(rest.substr(2)))
                                           : std::string());
            // The include guard wraps the whole file and its macro is already defined by
            // the time a split piece is compiled, so replaying it would delete the body.
            if (!guard_skipped && stack.empty() && !guard.empty()) {
                pending_guard = guard;
                guard_skipped = true;
                stack.push_back(std::string());   // placeholder, emitted as nothing
                continue;
            }
            stack.push_back(t);
        } else if (directive == "elif" || directive == "else") {
            if (!stack.empty() && !stack.back().empty()) {
                // The piece replays one branch on its own, so the branch it sits in has to
                // become an `#if` of its own: `#elif C` alone is not a valid opening
                // directive. The harvest already told us this branch is the live one, so
                // `#else` becomes `#if 1`.
                if (directive == "else") {
                    stack.back() = "#if 1";
                } else {
                    const size_t kw = rest.find("elif");
                    stack.back() = "#if " + trim_ws(rest.substr(kw + 4));
                }
            }
        } else if (directive == "endif") {
            if (!stack.empty()) stack.pop_back();
        } else if (directive == "define" && !pending_guard.empty() && ident == pending_guard) {
            pending_guard.clear();
        }
    }

    std::vector<std::string> out;
    for (const auto& c : stack)
        if (!c.empty()) out.push_back(c);
    return out;
}

// Macros the file undefines somewhere. A body that uses one of these cannot be moved into
// a split file: the piece includes the whole header first, so the #undef has already run by
// the time the body is compiled and the macro is gone. Boost.Assert defines
// BOOST_ASSERT_SNPRINTF, uses it in source_location::to_string, and undefines it a few
// lines later.
static std::set<std::string> undefined_macros(const std::string& source) {
    std::set<std::string> names;
    for (const auto& ll : logical_lines(source)) {
        std::string t = trim_ws(ll.text);
        if (t.empty() || t[0] != '#') continue;
        std::istringstream ls(trim_ws(t.substr(1)));
        std::string directive, ident;
        ls >> directive >> ident;
        if (directive == "undef" && !ident.empty()) names.insert(ident);
    }
    return names;
}

// Fills in the derived fields on every function: whether its definition has to stay in the
// header, and for class members the declaration left behind in the class plus the
// out-of-line definition written into the split file.
static bool is_header_file(const std::string& path);
static bool is_included_file(const std::string& path);

static void dump_keep_decisions(const std::vector<FunctionInfo>& functions) {
    const char* dump = std::getenv("CPP_SPLITTER_DUMP_HARVEST");
    if (!dump) return;
    for (const auto& fn : functions) {
        if (fn.file.find(dump) == std::string::npos) continue;
        fprintf(stderr, "[keep] line=%-5u keep=%d shared=%d ai=%d ranges=%zu tmpl=%d virt=%d ctor=%d spec=%d anon=%d %s\n",
                fn.start_line, (int)fn.keep_in_header, (int)fn.shares_extent,
                (int)!fn.always_inline_ranges.empty(),
                fn.always_inline_ranges.size(), (int)fn.is_template, (int)fn.is_virtual,
                (int)fn.is_ctor_or_dtor, (int)fn.is_specialization, (int)fn.in_unnamed_ns,
                fn.name.c_str());
    }
    fflush(stderr);
}

static bool extent_is_a_definition(const FunctionInfo& fn, const std::string& text);
static bool type_lacks_linkage(CXType type, int depth);

// Widen a macro-invocation fragment to the whole invocation.
//
// When a definition's tokens come from a macro body, libclang reports its extent inside the
// invocation -- and what lands there can be a fragment of it. Boost.Test writes
//
//     BOOST_TEST_SINGLETON_CONS_IMPL(collector_t)
//
// which expands to one member function, and the extent reported for that function is the
// two characters `t)`: the tail of the argument and the closing paren. Nothing can be done
// with a fragment. The invocation, however, is a unit -- it produced this definition and
// nothing else the file refers to -- so widening to it makes the definition movable again.
//
// Returns false when the text around `start` does not look like the inside of an
// invocation, in which case the caller leaves the definition alone.
static bool widen_to_macro_invocation(const std::string& blanked, unsigned& start, unsigned& end) {
    // Walk back to the `(` that opens the argument list.
    size_t i = start;
    int depth = 0;
    while (i > 0) {
        const char c = blanked[--i];
        if (c == ')') ++depth;
        else if (c == '(') {
            if (depth == 0) break;
            --depth;
        }
    }
    if (i == 0 || blanked[i] != '(') return false;
    const size_t open = i;

    // The macro's name sits immediately before it.
    while (i > 0 && std::isspace(static_cast<unsigned char>(blanked[i - 1]))) --i;
    const size_t name_end = i;
    while (i > 0 && is_ident_char(static_cast<unsigned char>(blanked[i - 1]))) --i;
    if (i == name_end) return false;
    if (std::isdigit(static_cast<unsigned char>(blanked[i]))) return false;

    // And forward to the `)` that closes it.
    size_t j = open;
    depth = 0;
    for (; j < blanked.size(); ++j) {
        if (blanked[j] == '(') ++depth;
        else if (blanked[j] == ')' && --depth == 0) break;
    }
    if (j >= blanked.size()) return false;

    start = static_cast<unsigned>(i);
    end = static_cast<unsigned>(j) + 1;
    return true;
}

static void prepare_functions(std::vector<FunctionInfo>& functions,
                              const std::string& source,
                              const std::set<std::string>& referenced,
                              bool input_is_header) {
    const std::set<std::string> undeffed = undefined_macros(source);

    // One extent, several definitions: the mark of a macro that expands to more than one
    // declaration. generate_preamble() folds such extents into one so the macro is not
    // written out once per definition it produced, which means splitting any one of them
    // would silently take the rest with it.
    //
    // The extents need not be *identical*, only overlapping. A macro argument is spelled in
    // the invocation, not in the macro body, so a definition whose first token comes from an
    // argument -- `D& assign(...)` in BOOST_STRONG_TYPEDEF -- has its extent start mapped to
    // where the argument was written, inside the invocation rather than at its beginning.
    // One macro then produces extents [invocation, end] and [argument, end], which overlap
    // and compare unequal.
    {
        std::vector<FunctionInfo*> by_extent;
        by_extent.reserve(functions.size());
        for (auto& fn : functions) by_extent.push_back(&fn);
        std::sort(by_extent.begin(), by_extent.end(),
                  [](const FunctionInfo* a, const FunctionInfo* b) {
                      if (a->start_offset != b->start_offset)
                          return a->start_offset < b->start_offset;
                      return a->end_offset < b->end_offset;
                  });
        size_t i = 0;
        while (i < by_extent.size()) {
            size_t j = i + 1;
            unsigned run_end = by_extent[i]->end_offset;
            while (j < by_extent.size() && by_extent[j]->start_offset < run_end) {
                run_end = std::max(run_end, by_extent[j]->end_offset);
                ++j;
            }
            if (j - i > 1)
                for (size_t k = i; k < j; ++k) by_extent[k]->shares_extent = true;
            i = j;
        }
    }

    // A macro that expands to exactly one out-of-line member definition. Its extent is a
    // fragment of the invocation; widening to the whole invocation makes it a unit that can
    // be moved to the definitions header, which is where a non-inline definition in a header
    // has to go. The restriction to a single out-of-line member is what makes the move safe:
    // the class already declares it, so nothing left in the file refers to the text.
    {
        const std::string blanked = blank_code_noise(source);
        for (auto& fn : functions) {
            if (fn.shares_extent || fn.defined_in_class) continue;
            if (extent_is_a_definition(fn, fn.body)) continue;
            bool is_member = false;
            for (const auto& e : fn.scope_chain)
                if (e.kind == ScopeKind::Class) { is_member = true; break; }
            if (!is_member) continue;

            unsigned start = fn.start_offset, end = fn.end_offset;
            if (!widen_to_macro_invocation(blanked, start, end)) continue;
            if (end > source.size()) continue;
            fn.start_offset = start;
            fn.end_offset = end;
            fn.body = source.substr(start, end - start);
            fn.macro_invocation = true;
        }
    }

    for (auto& fn : functions) {
        fn.conditionals = active_conditionals(source, fn.start_offset);

        if (fn.shares_extent) {
            fn.keep_in_header = true;
            continue;
        }

        if (!undeffed.empty()) {
            bool uses_undeffed = false;
            const std::string body_blanked = blank_code_noise(fn.body);
            for (const auto& m : undeffed) {
                if (contains_decl_token(body_blanked, m)) { uses_undeffed = true; break; }
            }
            // The same trap one level up. A split piece replays the conditionals the
            // definition was written under, but it replays them *after* including the whole
            // header -- so a condition testing a macro the header undefines at the end of
            // itself is false by then, the definition is preprocessed away, and the piece
            // compiles to an empty object. Boost.Core's demangle.hpp does exactly this:
            // it defines BOOST_CORE_HAS_CXXABI_H, writes demangle() under
            // `#if defined(BOOST_CORE_HAS_CXXABI_H)`, and undefines it again on the last
            // line. Nothing complains -- the object is simply empty and every caller is
            // left with an undefined reference.
            for (const auto& c : fn.conditionals) {
                if (uses_undeffed) break;
                const std::string cond_blanked = blank_code_noise(c);
                for (const auto& m : undeffed)
                    if (contains_decl_token(cond_blanked, m)) { uses_undeffed = true; break; }
            }
            if (uses_undeffed) {
                fn.uses_undefined_macro = true;
                fn.keep_in_header = true;
                continue;
            }
        }
        std::vector<std::string> class_names;
        for (const auto& e : fn.scope_chain)
            if (e.kind == ScopeKind::Class) class_names.push_back(e.name);
        const bool is_member = !class_names.empty() && fn.defined_in_class;

        // A function in an unnamed namespace cannot be split cleanly. Its definition would
        // have to leave the unnamed namespace to be reachable from another object, which is
        // what the rename is for -- but any forward declaration the source already wrote
        // inside that namespace gets renamed in place and stays there, leaving two
        // functions with one name in two scopes and making every call ambiguous. Keeping
        // the definition in the preamble costs a copy per object, which internal linkage
        // makes harmless, and keeps the meaning of the code intact.
        // A constructor or destructor is not one symbol but a family -- C1/C2/C3 and
        // D0/D1/D2 -- and only the compiler decides which members of it to emit. Split out
        // of a header the definition is `inline`, and `__attribute__((used))` pins exactly
        // one variant: the piece emits the base-object constructor while every caller,
        // which can no longer inline it, asks for the complete-object one, and the link
        // fails on a symbol that `nm -C` reports as present because both variants demangle
        // to the same text. Dropping `inline` would emit the whole family but make the
        // definition strong, which collides as soon as two translation units include the
        // header. So they stay put; construction is bound up with the class's fields
        // anyway, and leaving it beside them costs little.
        // Only split what this translation unit actually emits; see collect_emitted().
        if (input_is_header && !fn.usr.empty() && referenced.find(fn.usr) == referenced.end()) {
            fn.keep_in_header = true;
            continue;
        }

        // `main` is not an ordinary function. It may not be inline, and a split piece taken
        // out of a header is written `inline` so that several objects may carry it -- so a
        // `main` defined in an included file becomes `inline main`, which is ill-formed.
        // Boost.Test writes exactly that: unit_test_main.ipp defines main, and every test
        // that includes the framework in header-only mode picks it up. There is also nothing
        // to gain: a program has one main, and it can only be emitted once.
        if (fn.name == "main" && fn.scope_chain.empty()) {
            fn.keep_in_header = true;
            continue;
        }

        // Internal linkage in a header is not the same problem as internal linkage in a
        // .cpp, and the rename only solves the second. A split-out definition with internal
        // linkage has to become external, or no other piece can call it, and it has to be
        // renamed, or objects from different translation units collide when `ld -r` merges
        // them. build_static_rename_map() does that per split unit -- and a translation
        // unit is split into many units, one per header. So a `static` function defined in
        // one header and called from another is renamed in its own preamble and nowhere
        // else, and the caller's header is left calling a name that no longer exists.
        //
        // Keeping it costs a copy per piece, which internal linkage makes harmless: every
        // translation unit that includes the header already had its own copy. This is the
        // argument the unnamed-namespace rule below already makes, and an unnamed namespace
        // is in fact a special case of it -- both report CXLinkage_Internal.
        if (fn.is_static && is_included_file(fn.file)) {
            fn.keep_in_header = true;
            continue;
        }

        // Harmless today, because the visitor does not harvest conversion operators at all
        // (see there). It is here so that turning that on cannot also start emitting pieces
        // for them: `operator T()` names its type where a return type would go, so the
        // out-of-line form rebuilt from `return_type + qualified_name` comes out as
        // `bool C::operator bool()`.
        if (fn.is_conversion) {
            fn.keep_in_header = true;
            continue;
        }

        if (fn.is_template || fn.in_class_template || fn.in_anonymous_class ||
            fn.in_specialization_without_name ||
            fn.is_specialization || fn.is_virtual || fn.in_unnamed_ns ||
            (fn.is_ctor_or_dtor && is_included_file(fn.file))) {
            fn.keep_in_header = true;
            continue;
        }

        // Only now, with the cheap rules exhausted, is it worth walking the signature's
        // types. See the note in visitor().
        if (fn.fn_type.kind != CXType_Invalid) {
            bool lacks = type_lacks_linkage(clang_getResultType(fn.fn_type), 0);
            const int nargs = clang_getNumArgTypes(fn.fn_type);
            for (int i = 0; i < nargs && !lacks; ++i)
                lacks = type_lacks_linkage(clang_getArgType(fn.fn_type, i), 0);
            if (lacks) {
                fn.signature_lacks_linkage = true;
                fn.keep_in_header = true;
                continue;
            }
        }

        const std::string blanked = blank_code_noise(fn.body);
        const size_t decl_end = definition_decl_end(blanked);
        if (decl_end == std::string::npos) {
            // No body to move: `= default;`, `= delete;`, or something unrecognised.
            fn.keep_in_header = true;
            continue;
        }

        // constexpr and consteval definitions have to stay visible to every caller.
        const std::string decl_blanked = blanked.substr(0, decl_end);
        if (fn.is_constexpr ||
            contains_decl_token(decl_blanked, "constexpr") ||
            contains_decl_token(decl_blanked, "consteval")) {
            fn.keep_in_header = true;
            continue;
        }

        if (!is_member) {
            // The declaration left behind carries the default arguments, so the definition
            // must not repeat them -- a default argument may be given only once per scope.
            std::string decl = fn.body.substr(0, decl_end);
            const std::string db0 = blank_code_noise(decl);
            const size_t name0 = find_declarator(db0, fn.name);
            if (name0 != std::string::npos) {
                const size_t open0 = db0.find('(', name0 + fn.name.size());
                if (open0 != std::string::npos && strip_default_args(decl, open0))
                    fn.outlined_body = decl + fn.body.substr(decl_end);
            }
            fn.keep_in_header = false;
            continue;
        }

        std::string decl = fn.body.substr(0, decl_end);
        const std::string tail = fn.body.substr(decl_end);

        fn.member_decl = decl;   // what the class keeps, unchanged

        for (const char* kw : {"virtual", "static", "explicit", "friend"})
            decl = strip_decl_specifier(decl, kw);
        strip_virt_specifiers(decl);

        const std::string db = blank_code_noise(decl);
        const size_t name_pos = find_declarator(db, fn.name);
        if (name_pos == std::string::npos) {
            fn.keep_in_header = true;
            continue;
        }
        const size_t open = db.find('(', name_pos + fn.name.size());
        if (open == std::string::npos || !strip_default_args(decl, open)) {
            fn.keep_in_header = true;
            continue;
        }

        // Default arguments sit after the name, so name_pos is still valid here.
        std::string qualification;
        for (const auto& c : class_names) qualification += c + "::";

        // A leading return type is looked up in the enclosing namespace once the
        // definition is out-of-line, so a class-scoped name such as `result_type` or
        // `iterator` stops resolving. Moving it to a trailing return type puts the lookup
        // back inside the class, which fixes every such name at once.
        const std::string db2 = blank_code_noise(decl);
        std::string ret_src;
        size_t ret_begin = 0;
        if (!has_trailing_return(db2, open)) {
            ret_begin = skip_leading_attributes(db2);
            if (ret_begin < name_pos)
                ret_src = trim_ws(decl.substr(ret_begin, name_pos - ret_begin));
        }
        if (ret_src == "auto" || ret_src == "decltype(auto)") {
            // A deduced return type has to stay visible to callers.
            fn.keep_in_header = true;
            continue;
        }

        // The source text before the name is not just the return type: it also holds
        // function specifiers, and those are routinely macros (BOOST_FORCEINLINE and
        // friends) that cannot be told apart from a type name textually. libclang already
        // knows the return type, so use that and drop the specifier run -- `inline` and
        // inlining hints are meaningless on a definition that now lives alone in a .cpp.
        std::string ret = ret_src.empty() ? std::string() : fn.return_type;
        if (!ret_src.empty() &&
            (ret.empty() || ret.find("(anonymous") != std::string::npos ||
             ret.find("(lambda") != std::string::npos || ret.find("(unnamed") != std::string::npos)) {
            fn.keep_in_header = true;
            continue;
        }

        if (ret.empty()) {
            // Constructor, destructor or conversion operator: nothing to move.
            decl.insert(name_pos, qualification);
            fn.outlined_body = decl + tail;
        } else {
            // Keep the line count of the declarator stable so the #line directive that
            // precedes the body still points at the right source line.
            std::string newlines(std::count(decl.begin() + ret_begin,
                                            decl.begin() + name_pos, '\n'), '\n');
            fn.outlined_body = decl.substr(0, ret_begin) + "auto " + newlines +
                               qualification + decl.substr(name_pos) +
                               " -> " + ret + " " + tail;
        }
        fn.keep_in_header = false;
    }
}

// Narrows the harvested variables to the ones that will actually be moved, and gives each
// its source text.
//
// A variable that stays in the preamble needs no entry at all: it is ordinary text between
// the function extents and is copied through, which is what already happens. Only the ones
// that move need to be cut out, so anything this cannot handle confidently is dropped and
// left where it was -- less moving, nothing broken.
static std::string variable_declaration_head(const std::string& text);


// Whether a `template<...>` prefix precedes `start`.
//
// The prefix sits outside the cursor's extent -- the same thing that makes an explicit
// specialization's `template< >` invisible to the harvest. An out-of-line definition of a
// static data member of a class template is written that way:
//
//     template< int M > spinlock spinlock_pool< M >::pool_[ 41 ] = { ... };
//
// Moving the definition would leave `template< int M >` stranded in the preamble in front of
// whatever came next, and carry a definition into the definitions header that no longer has
// the template header it needs.
static bool has_template_prefix(const std::string& blanked, unsigned start) {
    size_t i = start;
    while (i > 0 && std::isspace(static_cast<unsigned char>(blanked[i - 1]))) --i;
    if (i == 0 || blanked[i - 1] != '>') return false;

    int depth = 0;
    while (i > 0) {
        const char c = blanked[--i];
        if (c == '>') ++depth;
        else if (c == '<' && --depth == 0) break;
    }
    if (depth != 0) return false;

    while (i > 0 && std::isspace(static_cast<unsigned char>(blanked[i - 1]))) --i;
    static const std::string kw = "template";
    if (i < kw.size()) return false;
    if (blanked.compare(i - kw.size(), kw.size(), kw) != 0) return false;
    return i == kw.size() ||
           !is_ident_char(static_cast<unsigned char>(blanked[i - kw.size() - 1]));
}

// A type that can be written in front of a name to declare it. Arrays and function pointers
// spell their declarator around the name (`int [3]`, `void (*)(int)`), so `extern <type>
// <name>;` does not work for them and the source text has to provide the declaration
// instead.
static bool type_spelling_is_simple(const std::string& type) {
    return !type.empty() &&
           type.find('[') == std::string::npos &&
           type.find('(') == std::string::npos;
}

// Said once per run, and only when a variable is actually moved. A definition that leaves
// the preamble is initialised in whichever object it lands in, and the order between objects
// is link order -- so anything reading it during static initialisation may see it empty.
// From C++17 the splitter avoids this by marking such definitions `inline` and leaving them
// where they are; before that, inline variables do not exist and there is nowhere else to put
// a definition that may exist only once.
static void warn_pre_cxx17_variable_move(const VariableInfo& var) {
    static bool said = false;
    if (said) return;
    said = true;
    // One line per translation unit, because that is how often this can fire on a large
    // build. Everything else is in the file it points at.
    std::cerr << "[cpp-splitter] warning: " << var.file << ": '" << var.name
              << "' moved to another object";
    if (g_cxx_standard > 0)
        std::cerr << " (C++" << (g_cxx_standard / 100) % 100 << "; needs C++17 to stay put)";
    std::cerr << "; its initialiser now runs in link order -- see example/static-init-order/\n";
}

static void prepare_variables(std::vector<VariableInfo>& variables,
                              const std::vector<FunctionInfo>& functions,
                              const std::string& source,
                              const std::string& unit_tag,
                              bool input_is_header) {
    const std::string blanked = blank_code_noise(source);

    // Extend every extent through its terminating semicolon. libclang stops at the
    // initialiser, and without the `;` the definition written into the definitions header
    // does not parse and the `;` left behind dangles after the declaration replacing it.
    std::vector<VariableInfo> usable;
    for (auto& var : variables) {
        // A `static` variable in the unit's own source is one object per *piece* if it stays
        // in the preamble, because every piece includes it -- not one per translation unit as
        // the language rule suggests. It is moved out and renamed, exactly as a `static`
        // function in a .cpp already is. TODO 26.
        //
        // Not from a header: two units splitting the same header separately would have to
        // agree on the mangled name, which is what TODO 23 declined. Not from an unnamed
        // namespace: hoisting the definition out changes the scope its name is looked up in,
        // and any declaration the source already wrote inside stays behind -- the same reason
        // prepare_functions() keeps unnamed-namespace functions.
        // `is_const` is what keeps a compile-time constant in place. Moving one leaves an
        // `extern` declaration where its users need a constant expression, and every use in
        // an array bound, a template argument or another constexpr initialiser stops
        // compiling. Boost.Filesystem's dir_itr_imp_extra_data_alignment is one, spelled
        // BOOST_CONSTEXPR_OR_CONST, which is why the text cannot be trusted to find them.
        if (!var.move_out && var.internal_linkage && !var.is_const && !input_is_header &&
            !var.in_unnamed_ns && !var.is_member && !var.name.empty()) {
            var.rename_and_move = true;
            var.move_out = true;
        }
        if (!var.move_out) continue;
        if (var.end_offset > source.size() || var.start_offset >= var.end_offset) continue;
        if (has_template_prefix(blanked, var.start_offset)) continue;
        unsigned end = var.end_offset;
        if (!extend_through_semicolon(blanked, end)) continue;
        var.end_offset = end;
        usable.push_back(std::move(var));
    }

    // Declarators of one declaration now share an end offset. `int a = 1, b = 2;` is one
    // definition of two variables: it moves whole or not at all, and the preamble gets an
    // `extern` for each name.
    std::sort(usable.begin(), usable.end(),
              [](const VariableInfo& a, const VariableInfo& b) {
                  if (a.end_offset != b.end_offset) return a.end_offset < b.end_offset;
                  return a.start_offset < b.start_offset;
              });

    std::vector<VariableInfo> kept;
    size_t i = 0;
    while (i < usable.size()) {
        size_t j = i + 1;
        while (j < usable.size() && usable[j].end_offset == usable[i].end_offset) ++j;

        VariableInfo group = usable[i];
        group.start_offset = usable[i].start_offset;
        group.text = source.substr(group.start_offset, group.end_offset - group.start_offset);

        // `inline` gives a variable vague linkage, so it may -- and must -- appear in every
        // piece. libclang reports it as ordinary external linkage, and does not print it
        // back in the pretty-printed declaration either, so the source text is the only
        // place it can be read.
        const std::string head = variable_declaration_head(group.text);
        const std::string head_blanked = blank_code_noise(head);
        if (contains_decl_token(head_blanked, "inline") ||
            contains_decl_token(head_blanked, "constexpr")) {
            i = j;
            continue;
        }

        // From C++17 there is a better placement than the definitions header: leave the
        // definition exactly where it is and mark it `inline`. The linker merges the copies
        // into one object, and -- the point -- it is still initialised in source order
        // relative to the code around it, because it never left. Moving it to another object
        // puts its dynamic initialiser in link order relative to anything that reads it
        // during static initialisation, which is a silently wrong program rather than a
        // failed build. example/static-init-order/ is three files that show it.
        //
        // `struct foo { ... } f;` is excluded: `inline` in front of a declaration that also
        // defines a type is not something to write, so that shape keeps the declarator-only
        // treatment below.
        const bool defines_a_type_here =
            group.name_offset > group.start_offset &&
            group.name_offset < group.end_offset &&
            blank_code_noise(source.substr(group.start_offset,
                                           group.name_offset - group.start_offset))
                    .find('{') != std::string::npos;

        // `inline` gives vague linkage, which is the opposite of what a variable with
        // internal linkage must have, so the rename path is checked first.
        if (g_cxx_standard >= 201703 && !defines_a_type_here && !group.rename_and_move) {
            group.inline_in_place = true;
            group.replacement.clear();
            kept.push_back(std::move(group));
            i = j;
            continue;
        }

        // Only when the move is forced by the standard. A variable with internal linkage is
        // renamed and moved whatever the standard, because that is the correct treatment for
        // it rather than a compromise -- `inline` would give it vague linkage, which is the
        // opposite of what it must have.
        if (!group.rename_and_move && g_cxx_standard > 0 && g_cxx_standard < 201703)
            warn_pre_cxx17_variable_move(group);

        bool usable_group = true;
        if (j - i == 1) {
            // `struct foo { ... } f;` defines a type and a variable in one declaration. The
            // type has to stay in the preamble -- every piece needs it -- while the variable
            // has to leave, so the declaration is cut at the declarator's name: the type
            // definition and its `;` stay, an `extern` follows, and the definitions header
            // gets the variable rebuilt from its type.
            const bool defines_a_type =
                group.name_offset > group.start_offset &&
                group.name_offset < group.end_offset &&
                blank_code_noise(source.substr(group.start_offset,
                                               group.name_offset - group.start_offset))
                        .find('{') != std::string::npos;

            if (group.is_member) {
                group.replacement.clear();          // the class already declares it
            } else if (defines_a_type) {
                // Take only the declarator. The type definition in front of it is left as
                // ordinary text -- which matters for more than tidiness: a member function
                // defined in that type is a definition of its own, and a range covering it
                // would swallow the declaration the splitter left in its place.
                if (type_spelling_is_simple(group.type_spelling)) {
                    group.start_offset = group.name_offset;
                    group.text = group.type_spelling + " " +
                                 source.substr(group.name_offset,
                                               group.end_offset - group.name_offset);
                    group.replacement = ";\nextern " + group.type_spelling + " " +
                                        group.name + ";";
                } else {
                    usable_group = false;
                }
            } else if (group.rename_and_move) {
                // Renamed like a static function: `static` comes off so the other pieces can
                // refer to it, and the name is mangled because it now has external linkage
                // and could collide with another unit that spells it the same way. The
                // definition itself is rewritten by apply_static_renames(); only the
                // declaration left behind has to name the mangled form directly, because
                // `replacement` goes into the preamble verbatim.
                if (type_spelling_is_simple(group.type_spelling)) {
                    group.text = strip_decl_specifier(group.text, "static");
                    group.replacement =
                        "extern " + group.type_spelling + " " +
                        make_static_mangled_name(unit_tag, group.name) + ";";
                } else {
                    usable_group = false;
                }
            } else if (!head.empty()) {
                // The source text spells the type exactly as written, arrays and function
                // pointers included.
                group.replacement = terminate_declaration("extern " + head);
            } else if (type_spelling_is_simple(group.type_spelling)) {
                group.replacement = "extern " + group.type_spelling + " " + group.name + ";";
            } else {
                usable_group = false;
            }
        } else {
            // Several declarators: the text gives no per-name declaration, so each is
            // rebuilt from its type. A declarator whose type cannot be written that way
            // takes the whole declaration out of the running -- it moves whole or not at
            // all, and half of it moving would be worse than none.
            for (size_t k = i; k < j && usable_group; ++k) {
                if (usable[k].is_member || usable[k].rename_and_move ||
                    !type_spelling_is_simple(usable[k].type_spelling))
                    usable_group = false;
            }
            if (usable_group) {
                group.replacement.clear();
                for (size_t k = i; k < j; ++k)
                    group.replacement +=
                        "extern " + usable[k].type_spelling + " " + usable[k].name + ";\n";
            }
        }

        // Nothing that can be moved safely. It stays in the preamble, where every piece
        // gets a copy -- which is the defect, so the link fails and the unit falls back to
        // compiling whole. That is the safe direction: slower, never wrong.
        if (usable_group) kept.push_back(std::move(group));
        i = j;
    }

    // A variable's range must not contain a function's. generate_preamble() folds
    // overlapping ranges and emits the union verbatim, which would put back a definition
    // that prepare_functions() decided to split -- and the piece for it is compiled either
    // way, so the definition would exist twice.
    std::vector<VariableInfo> disjoint;
    for (auto& var : kept) {
        bool overlaps = false;
        for (const auto& fn : functions) {
            if (fn.start_offset < var.end_offset && var.start_offset < fn.end_offset) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) disjoint.push_back(std::move(var));
    }
    variables.swap(disjoint);
}

// The declarator part of a variable definition: everything before the initialiser, in
// whichever of its three spellings the source used. Empty when the text does not look like
// one declaration of one variable -- `int a = 0, b = 1;` declares two, and neither can be
// moved without the other.
static std::string variable_declaration_head(const std::string& text) {
    const std::string blanked = blank_code_noise(text);
    int depth = 0;
    for (size_t i = 0; i < blanked.size(); ++i) {
        const char c = blanked[i];
        if (depth == 0) {
            if (c == '=' || c == '(' || c == '{' || c == ';')
                return trim_ws(text.substr(0, i));
            if (c == ',') return "";
        }
        if (c == '(' || c == '[' || c == '{' || c == '<') ++depth;
        else if (c == ')' || c == ']' || c == '}' || c == '>') --depth;
    }
    return "";
}

// Whether a definition's extent text is actually a definition.
//
// It is not when the tokens came from a macro body: libclang then reports the extent in the
// invocation, and what lands there can be a fragment of it. BOOST_TEST_SINGLETON_CONS_IMPL
// expands to one member function, and the extent libclang gives for it is the two characters
// `t)` -- the tail of `BOOST_TEST_SINGLETON_CONS_IMPL(collector_t)`. Moving that anywhere
// leaves the invocation unterminated, and the next `#include` in the file is then read as a
// macro argument:
//
//     error: embedding a #include directive within macro arguments is not supported
//
// A brace means a body was written here. Failing that, the declarator's own name appearing
// in the text means the declaration was: `Foo::Foo() = default;` has no brace and is still a
// definition that can be moved.
static bool extent_is_a_definition(const FunctionInfo& fn, const std::string& text) {
    const std::string blanked = blank_code_noise(text);
    if (definition_decl_end(blanked) != std::string::npos) return true;
    return !fn.name.empty() && find_declarator(blanked, fn.name) != std::string::npos;
}

// Builds the preamble, and separates out the definitions that must not be repeated.
//
// The preamble is included by every split piece, so everything it carries is compiled once
// per piece. That is harmless for declarations, and for definitions with vague linkage,
// which is what the language provides for exactly this. It is not harmless for an ordinary
// definition with external linkage: each piece emits a strong symbol and `ld -r` rejects
// the result.
//
// Such definitions are collected into `definitions` instead, which the caller writes to a
// second header included by exactly one piece. Splitting the preamble this way makes "safe
// to include many times" a property of how it is built, rather than something every keep
// rule has to get right on its own.
static std::string wrap_in_namespaces(const std::string& body,
                                      const std::vector<ScopeEntry>& scope_chain);

static std::string generate_preamble(const std::string& source,
                                     const std::vector<FunctionInfo>& functions,
                                     const std::vector<VariableInfo>& variables,
                                     const std::string& stem,
                                     const std::string& source_path,
                                     const StaticRenameMap& renames,
                                     std::string* definitions = nullptr) {
    struct Range {
        unsigned start, end;
        bool keep;
        const FunctionInfo* fn;
        const VariableInfo* var;
    };
    std::vector<Range> ranges;
    for (const auto& fn : functions) {
        ranges.push_back({fn.start_offset, fn.end_offset, should_keep_in_header(fn), &fn, nullptr});
    }
    // Only the variables that move are listed. One that stays is ordinary text between the
    // function extents and is copied through without an entry, which is what it already was.
    for (const auto& var : variables) {
        ranges.push_back({var.start_offset, var.end_offset, false, nullptr, &var});
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const Range& a, const Range& b) {
                  if (a.start != b.start) return a.start < b.start;
                  if (a.end != b.end) return a.end < b.end;
                  return a.keep && !b.keep;   // a kept entry represents the group
              });

    // Several functions can share one source extent: a macro such as BOOST_BITMASK expands
    // to a whole set of operators, and every one of them reports the macro invocation as its
    // extent. Emitting the retained text once per function would repeat the macro and
    // redefine everything it declares.
    //
    // Overlapping is enough; identical is not required. When a definition's first token
    // comes from a macro *argument*, libclang maps its extent start into the invocation
    // rather than to its beginning, so one macro yields extents that overlap and compare
    // unequal. Collapsing only the identical ones left the emit loop re-emitting the tail:
    //
    //     BOOST_STRONG_TYPEDEF(unsigned int, version_type)
    //     version_type)
    //
    // A run of overlapping extents is by construction unsplittable -- no one declarator
    // covers it, so there is no declaration to leave behind and nothing to move. Emitting
    // the union verbatim reproduces the source exactly, which is what the identical-extent
    // collapse achieved for the easy case.
    {
        std::vector<Range> merged;
        for (const auto& r : ranges) {
            if (!merged.empty() && r.start < merged.back().end) {
                Range& prev = merged.back();
                if (r.end > prev.end) prev.end = r.end;
                prev.keep = true;
                prev.fn = nullptr;
                prev.var = nullptr;
                continue;
            }
            merged.push_back(r);
        }
        ranges.swap(merged);
    }

    // The emit loop's correctness depends on this and nothing else states it: the ranges
    // must be strictly increasing and non-overlapping, or text is duplicated or dropped.
    for (size_t i = 1; i < ranges.size(); ++i)
        assert(ranges[i].start >= ranges[i - 1].end && "preamble ranges must not overlap");

    auto line_offsets = build_line_offsets(source);

    std::string preamble;
    preamble += "#pragma once\n";
    //preamble += "#line 1 \"" + source_path + "\"\n";

    auto ensure_newline = [&preamble]() {
        if (!preamble.empty() && preamble.back() != '\n')
            preamble += '\n';
    };

    unsigned pos = 0;
    for (const auto& r : ranges) {
        if (r.start > pos) {
            preamble += apply_static_renames(source.substr(pos, r.start - pos), renames);
        }
        if (r.keep) {
            ensure_newline();
            unsigned keep_line = offset_to_line(line_offsets, r.start);
            //preamble += "#line " + std::to_string(keep_line) + " \"" + source_path + "\"\n";
            // A retained template body can call a split-out static function too.
            std::string text = apply_static_renames(
                strip_always_inline(source.substr(r.start, r.end - r.start), r.start,
                                    r.fn ? r.fn->always_inline_ranges
                                         : std::vector<std::pair<unsigned, unsigned>>()),
                renames);

            // A definition that needs a macro the file retracts cannot go to the
            // definitions header: that file includes the preamble first, and the `#undef`
            // has run by then. undefined_macros() already keeps such a definition out of a
            // *piece* for the same reason (TODO/16); the rule simply never reached this
            // second placement decision. It stays here instead, emitted `inline` so that the
            // copy every piece gets merges rather than colliding -- which is the same
            // treatment a definition split out of a header already receives.
            if (definitions && r.fn && !has_vague_linkage(*r.fn) &&
                !r.fn->uses_undefined_macro &&
                (r.fn->macro_invocation || extent_is_a_definition(*r.fn, text))) {
                // Kept, but it may exist in only one object. Out of the shared preamble it
                // goes; a member is already declared by its class, and a free function gets
                // a declaration left where its body was. The text is lifted out of whatever
                // namespaces enclosed it, so they have to be reopened around it.
                *definitions += wrap_in_namespaces(text, r.fn->scope_chain);
                *definitions += "\n";
                if (r.fn->member_decl.empty()) {
                    std::string decl = generate_forward_decl_inplace(*r.fn, stem);
                    if (!decl.empty()) preamble += decl + "\n";
                }
            } else {
                if (r.fn && r.fn->uses_undefined_macro && !has_vague_linkage(*r.fn)) {
                    const std::string blanked = blank_code_noise(text);
                    const size_t at = inline_insertion_point(blanked);
                    if (at != std::string::npos &&
                        !contains_decl_token(blanked.substr(0, decl_prefix_end(blanked)),
                                             "inline"))
                        text.insert(at, at == 0 ? "inline " : " inline");
                }
                preamble += text;
            }
        } else if (r.var) {
            // A variable that may exist in only one object. It goes to the definitions
            // header, reopened in its namespaces, and an `extern` declaration takes its
            // place -- at the position it occupied, so it precedes every use the file had.
            // An out-of-class static data member needs no declaration: its class has one.
            ensure_newline();
            if (r.var->inline_in_place) {
                // C++17: it stays, marked `inline`, so the copies merge and the order
                // relative to its neighbours is the order the source had.
                std::string text = apply_static_renames(r.var->text, renames);
                const std::string blanked = blank_code_noise(text);
                if (!contains_decl_token(blanked, "inline"))
                    text.insert(0, "inline ");
                preamble += text;
            } else if (!definitions) {
                // No definitions header to move it to: leave it exactly where it was.
                preamble += apply_static_renames(r.var->text, renames);
            } else {
                *definitions += wrap_in_namespaces(
                    apply_static_renames(r.var->text, renames), r.var->scope_chain);
                *definitions += "\n";
                if (!r.var->replacement.empty()) preamble += r.var->replacement + "\n";
            }
        } else if (r.fn) {
            // The definition moved to a split file, so a declaration has to take its
            // place: inside the class for a member, otherwise right here, where the
            // definition used to be.
            ensure_newline();
            if (!r.fn->member_decl.empty())
                preamble += terminate_declaration(
                    apply_static_renames(r.fn->member_decl, renames));
            else {
                // Only a renamed function needs its declaration here. Its name changed, so
                // a use in a namespace-scope initialiser retained in the preamble -- the
                // `fn_ptr = &impl;` idiom -- refers to a name nothing has declared yet, and
                // a declaration appended at the end of the preamble comes far too late.
                std::string decl = generate_forward_decl_inplace(*r.fn, stem);
                if (!decl.empty()) {
                    // The split definition is hoisted out of any unnamed namespace, so the
                    // declaration has to leave it too, or the two get different linkage and
                    // the reference goes unresolved. Closing and reopening the unnamed
                    // namespace puts the declaration in the definition's scope while still
                    // keeping it ahead of every use.
                    for (unsigned i = 0; i < r.fn->unnamed_ns_depth; ++i) preamble += "}\n";
                    preamble += decl + "\n";
                    for (unsigned i = 0; i < r.fn->unnamed_ns_depth; ++i) preamble += "namespace {\n";
                }
            }
        }
        ensure_newline();
        unsigned resume_line = offset_to_line(line_offsets, r.end);
        //preamble += "#line " + std::to_string(resume_line) + " \"" + source_path + "\"\n";
        pos = r.end;
    }
    if (pos < source.size()) {
        preamble += apply_static_renames(source.substr(pos), renames);
    }

    preamble += "\n";

    return preamble;
}

static std::string wrap_in_namespaces(const std::string& body,
                                      const std::vector<ScopeEntry>& scope_chain) {
    std::vector<std::string> ns_names;
    for (const auto& entry : scope_chain) {
        if (entry.kind == ScopeKind::Namespace)
            ns_names.push_back(entry.name);
        else
            break;
    }

    if (ns_names.empty())
        return body;

    std::string result;
    for (const auto& ns : ns_names) {
        result += "namespace " + ns + " {\n";
    }
    result += "\n" + body + "\n";
    for (size_t i = 0; i < ns_names.size(); ++i) {
        result += "}\n";
    }
    return result;
}

namespace bp = boost::process;

static int run_command(const std::string& cmd) {
    std::cout << "  $ " << cmd << "\n";
    int ret = std::system(cmd.c_str());
    return WEXITSTATUS(ret);
}

static int run_command_quiet(const std::string& cmd) {
    int ret = std::system(cmd.c_str());
    return WEXITSTATUS(ret);
}

struct CompileJob {
    std::string cmd;
    std::string source_file;
    std::string obj_file;
};

struct CompileResult {
    int exit_code;
    std::string source_file;
    std::string stderr_output;
};

static unsigned get_parallelism() {
    unsigned hw = std::thread::hardware_concurrency();
    return hw > 0 ? hw : 4;
}

static std::vector<CompileResult> compile_parallel(const std::vector<CompileJob>& jobs,
                                                    bool verbose,
                                                    std::ostream& out = std::cout) {
    std::vector<CompileResult> results(jobs.size());
    std::mutex output_mtx;
    std::vector<std::thread> threads;
    std::atomic<size_t> next_job{0};

    unsigned num_threads = std::min(static_cast<unsigned>(jobs.size()), get_parallelism());

    if (verbose) {
        std::lock_guard<std::mutex> lock(output_mtx);
        out << "  [parallel: " << num_threads << " threads, "
            << jobs.size() << " jobs]\n\n";
    }

    for (unsigned t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            while (true) {
                size_t idx = next_job.fetch_add(1);
                if (idx >= jobs.size()) break;

                const auto& job = jobs[idx];
                auto& res = results[idx];
                res.source_file = job.source_file;

                try {
                    bp::ipstream err_stream;
                    bp::child proc("/bin/sh", bp::args({"-c", job.cmd}),
                                   bp::std_out > bp::null,
                                   bp::std_err > err_stream);

                    std::string stderr_buf;
                    std::string line;
                    while (std::getline(err_stream, line)) {
                        stderr_buf += line + "\n";
                    }

                    proc.wait();
                    res.exit_code = proc.exit_code();
                    res.stderr_output = stderr_buf;
                } catch (const std::exception& e) {
                    res.exit_code = 1;
                    res.stderr_output = std::string("boost::process error: ") + e.what() + "\n";
                }

                if (verbose) {
                    std::lock_guard<std::mutex> lock(output_mtx);
                    out << "  $ " << job.cmd << "\n";
                    if (!res.stderr_output.empty()) {
                        std::cerr << res.stderr_output;
                    }
                }
            }
        });
    }

    for (auto& th : threads)
        th.join();

    return results;
}

static std::string file_content_hash(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    size_t h = std::hash<std::string>{}(ss.str());
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016zx", h);
    return std::string(buf);
}

static bool needs_recompile(const std::string& cpp_file, const std::string& obj_file,
                            const std::string& preamble_file = "") {
    if (!fs::exists(obj_file)) return true;
    auto obj_time = fs::last_write_time(obj_file);
    if (fs::last_write_time(cpp_file) > obj_time) return true;
    if (!preamble_file.empty() && fs::exists(preamble_file)) {
        if (fs::last_write_time(preamble_file) > obj_time) return true;
        std::string gch_dir = preamble_file + ".gch";
        if (fs::is_directory(gch_dir)) {
            for (const auto& entry : fs::directory_iterator(gch_dir)) {
                if (fs::last_write_time(entry.path()) > obj_time)
                    return true;
            }
        }
    }
    return false;
}

static bool is_source_file(const std::string& path) {
    static const char* exts[] = {".cpp", ".cc", ".cxx", ".C", ".c++", ".cp", ".c"};
    for (const char* ext : exts) {
        size_t elen = std::strlen(ext);
        if (path.size() >= elen && path.compare(path.size() - elen, elen, ext) == 0)
            return true;
    }
    return false;
}

static bool is_header_file(const std::string& path) {
    static const char* exts[] = {".h", ".hpp", ".hxx", ".H", ".h++", ".hh"};
    for (const char* ext : exts) {
        size_t elen = std::strlen(ext);
        if (path.size() >= elen && path.compare(path.size() - elen, elen, ext) == 0)
            return true;
    }
    return false;
}

// A file that is included into a translation unit rather than compiled as one. Boost puts
// real definitions in implementation includes -- boost/detail/utf8_codecvt_facet.ipp holds
// the whole of utf8_codecvt_facet, and three libraries include it into a source of their
// own -- so for every rule that turns on "is this definition in a header", a .ipp is one.
static bool is_included_file(const std::string& path) {
    if (is_header_file(path)) return true;
    static const char* exts[] = {".ipp", ".inl", ".inc", ".tcc"};
    for (const char* ext : exts) {
        size_t elen = std::strlen(ext);
        if (path.size() >= elen && path.compare(path.size() - elen, elen, ext) == 0)
            return true;
    }
    return false;
}

static bool is_splittable_file(const std::string& path) {
    return is_source_file(path) || is_header_file(path);
}

static std::string shell_quote(const std::string& s) {
    if (s.find_first_of(" \t\n'\"\\$`!#&|;(){}[]<>?*~") == std::string::npos)
        return s;
    std::string result = "'";
    for (char c : s) {
        if (c == '\'')
            result += "'\\''";
        else
            result += c;
    }
    result += "'";
    return result;
}

static bool build_pch(const std::string& preamble_file,
                      const std::string& compiler_driver,
                      const std::string& compiler,
                      const std::vector<std::string>& flags,
                      const std::string& include_dir,
                      bool verbose,
                      std::ostream& out = std::cout) {
    std::string hash = file_content_hash(preamble_file);
    if (hash.empty()) {
        if (verbose) out << "  PCH: cannot read preamble, skipping\n";
        return false;
    }

    std::string gch_dir = preamble_file + ".gch";
    std::string gch_file = gch_dir + "/" + hash + ".gch";

    if (fs::exists(gch_file)) {
        if (verbose) out << "  PCH up-to-date: " << gch_file << "\n";
        return true;
    }

    if (fs::exists(gch_dir)) {
        for (const auto& entry : fs::directory_iterator(gch_dir))
            fs::remove(entry.path());
    } else {
        fs::create_directories(gch_dir);
    }

    std::string cmd = shell_quote(compiler_driver) + " " + shell_quote(compiler) + " -x c++-header";
    for (const auto& f : flags)
        cmd += " " + shell_quote(f);
    if (!include_dir.empty())
        cmd += " -I" + shell_quote(include_dir);
    cmd += " -o " + shell_quote(gch_file) + " " + shell_quote(preamble_file);

    if (verbose) out << "  Building PCH: " << cmd << "\n";
    int ret = run_command_quiet(cmd);
    if (ret != 0) {
        if (verbose) out << "  PCH build failed (exit " << ret << "), continuing without PCH\n";
        fs::remove_all(gch_dir);
        return false;
    }

    if (verbose) out << "  PCH built: " << gch_file << "\n";
    return true;
}

// The source's include prefix: everything up to the first construct that is not a
// preprocessor directive, comment or blank line.
//
// This is where a parse spends its time. A Boost translation unit is a few hundred lines of
// its own behind tens of thousands of lines of headers, and every launcher invocation
// re-reads all of them. Precompiling the prefix and feeding it back turns that into a load.
//
// It is specifically the *prefix*, not the preamble: the preamble is the source with its
// function bodies carved out, so it declares everything the source declares, and feeding its
// precompiled form back into a parse of that same source redefined all of it -- which is why
// TODO 08 had to stop doing exactly that. A prefix declares nothing of the source's own, so
// including it is only what the compiler was going to do anyway, already parsed.
static std::string include_prefix_of(const std::string& source) {
    const std::string blanked = blank_code_noise(source);
    size_t pos = 0, cut = 0;
    int conditional_depth = 0;

    while (pos < blanked.size()) {
        size_t eol = blanked.find('\n', pos);
        if (eol == std::string::npos) eol = blanked.size();
        const std::string line = trim_ws(blanked.substr(pos, eol - pos));

        if (!line.empty()) {
            if (line[0] == '#') {
                std::istringstream ls(trim_ws(line.substr(1)));
                std::string directive;
                ls >> directive;
                if (directive == "if" || directive == "ifdef" || directive == "ifndef")
                    ++conditional_depth;
                else if (directive == "endif" && conditional_depth > 0)
                    --conditional_depth;
            } else {
                // Real code. Stop here, and if it is inside a conditional stop before that
                // conditional opened rather than cutting the block in half.
                break;
            }
            // Only cut at a point where every conditional that was opened has been closed,
            // so the prefix is always balanced.
            if (conditional_depth == 0) cut = eol < blanked.size() ? eol + 1 : eol;
        }
        pos = eol + 1;
    }
    return source.substr(0, cut);
}

static void inclusion_visitor(CXFile included_file, CXSourceLocation* stack,
                              unsigned include_len, CXClientData client_data);

// Whether a precompiled header is still good for the files it was built from.
//
// The PCH is named after a hash of the prefix file -- the include directives at the top of
// the source. That text does not change when one of the headers it names is edited, so the
// name alone says nothing about whether the PCH still matches what is on disk. Feeding a
// stale one back to libclang does not degrade gracefully: the parse fails outright with
// CXError_ASTReadError, the unit falls back to compiling whole, and the fallback is silent
// unless someone is reading the verbose log. It made every header edit in the Spirit
// benchmark measure a fallback rather than a split.
//
// So the PCH records what it was built from, and is rebuilt when any of that is newer.
static bool pch_is_current(const std::string& pch_file) {
    const std::string deps_file = pch_file + ".deps";
    if (!fs::exists(deps_file)) return false;   // built before this check existed

    std::error_code ec;
    const auto pch_time = fs::last_write_time(pch_file, ec);
    if (ec) return false;

    std::ifstream ifs(deps_file);
    if (!ifs.is_open()) return false;
    std::string dep;
    while (std::getline(ifs, dep)) {
        if (dep.empty()) continue;
        const auto dep_time = fs::last_write_time(dep, ec);
        if (ec) return false;                   // vanished: rebuild and find out
        if (dep_time > pch_time) return false;
    }
    return true;
}

static std::string build_libclang_pch(const std::string& preamble_file,
                                       const std::vector<std::string>& clang_flags,
                                       bool verbose,
                                       std::ostream& out = std::cout) {
    if (!fs::exists(preamble_file)) return "";

    std::string hash = file_content_hash(preamble_file);
    if (hash.empty()) return "";

    std::string pch_dir = preamble_file + ".pch";
    std::string pch_file = pch_dir + "/" + hash + ".pch";

    if (fs::exists(pch_file)) {
        if (pch_is_current(pch_file)) {
            if (verbose) out << "  libclang PCH up-to-date: " << pch_file << "\n";
            return pch_file;
        }
        if (verbose) out << "  libclang PCH stale, rebuilding: " << pch_file << "\n";
    }

    if (fs::exists(pch_dir)) {
        for (const auto& entry : fs::directory_iterator(pch_dir))
            fs::remove(entry.path());
    } else {
        fs::create_directories(pch_dir);
    }

    CXIndex index = clang_createIndex(0, 0);
    if (!index) {
        if (verbose) out << "  libclang PCH: failed to create index\n";
        return "";
    }

    std::vector<std::string> pch_flags;
    for (const auto& f : clang_flags) {
        if (f == "-fsyntax-only") continue;
        pch_flags.push_back(f);
    }
    if (std::find(pch_flags.begin(), pch_flags.end(), "-x") == pch_flags.end()) {
        pch_flags.insert(pch_flags.begin(), {"-x", "c++-header"});
    }

    std::vector<const char*> args;
    for (const auto& f : pch_flags) args.push_back(f.c_str());

    CXTranslationUnit tu = nullptr;
    CXErrorCode err = clang_parseTranslationUnit2(
        index, preamble_file.c_str(), args.data(),
        static_cast<int>(args.size()), nullptr, 0, CXTranslationUnit_ForSerialization, &tu);

    if (err != CXError_Success || !tu) {
        if (verbose) out << "  libclang PCH: parse failed (code: " << err << ")\n";
        clang_disposeIndex(index);
        fs::remove_all(pch_dir);
        return "";
    }

    int save_err = clang_saveTranslationUnit(tu, pch_file.c_str(), clang_defaultSaveOptions(tu));

    // What this PCH is only valid for. Written before the translation unit is disposed,
    // because that is the only place the include closure is known.
    if (save_err == CXSaveError_None) {
        std::vector<std::string> includes;
        clang_getInclusions(tu, inclusion_visitor, &includes);
        std::ofstream deps(pch_file + ".deps");
        if (deps.is_open()) {
            deps << preamble_file << "\n";
            for (const auto& inc : includes) deps << inc << "\n";
        }
    }

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);

    if (save_err != CXSaveError_None) {
        if (verbose) out << "  libclang PCH: save failed (code: " << save_err << ")\n";
        fs::remove_all(pch_dir);
        return "";
    }

    if (verbose) out << "  libclang PCH built: " << pch_file << "\n";
    return pch_file;
}

struct SplitResult {
    std::vector<std::string> compilable_files;
    std::string preamble_filename;
    // Must be initialised here. `SplitResult sr;` at block scope leaves a bare bool
    // indeterminate, and both split drivers are reached through `if (!sr.success)`: when the
    // stack happened to hold a non-zero byte, splitting was skipped entirely and the tool
    // exited 0 having silently done nothing. It showed up as roughly one run in six.
    bool success = false;
    std::vector<std::string> header_obj_dirs;
    std::vector<std::string> header_obj_files;
};

struct SplitHeaderInfo {
    std::string split_dir;
    std::string preamble_path;
    std::vector<std::string> compilable_files;
};

static std::unordered_map<std::string, SplitHeaderInfo> g_split_headers;


static void inclusion_visitor(CXFile included_file, CXSourceLocation* /*stack*/,
                              unsigned /*include_len*/, CXClientData client_data) {
    auto* includes = static_cast<std::vector<std::string>*>(client_data);
    std::string path = cx_to_string(clang_getFileName(included_file));
    if (!path.empty()) {
        std::error_code ec;
        std::string abs = fs::absolute(path, ec).string();
        if (!ec) includes->push_back(abs);
    }
}

static const std::vector<std::string>& cached_system_includes();

static bool is_stdlib_header(const std::string& abs_path) {
    const auto& stdlib_paths = cached_system_includes();
    for (const auto& sp : stdlib_paths) {
        if (abs_path.size() >= sp.size() &&
            abs_path.compare(0, sp.size(), sp) == 0)
            return true;
    }
    return false;
}

// --- Split output layout -------------------------------------------------------------
//
// Split headers were once written flat, one file per basename, into the translation
// unit's .split directory. Two headers sharing a basename then overwrote each other, and
// the survivor shadowed the original for every consumer, because that directory is placed
// first on the include path. Headers are now mirrored under <split_dir>/include at the
// path they were included as, so the rewritten tree is structurally interchangeable with
// the real include directories and no two headers can claim the same output path.

static std::vector<std::string> include_dirs_from_flags(const std::vector<std::string>& flags) {
    std::set<std::string> unique_dirs;
    for (size_t i = 0; i < flags.size(); ++i) {
        const std::string& f = flags[i];
        std::string dir;
        if (f == "-I" || f == "-isystem" || f == "-iquote") {
            if (i + 1 < flags.size()) dir = flags[++i];
        } else if (f.rfind("-I", 0) == 0 && f.size() > 2) {
            dir = f.substr(2);
        } else if (f.rfind("-isystem", 0) == 0 && f.size() > 8) {
            dir = f.substr(8);
        }
        if (dir.empty()) continue;
        std::error_code ec;
        std::string abs = fs::absolute(dir, ec).lexically_normal().string();
        if (!ec) unique_dirs.insert(abs);
    }
    std::vector<std::string> dirs(unique_dirs.begin(), unique_dirs.end());
    // Longest first: the most specific include directory must win the match.
    std::sort(dirs.begin(), dirs.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    return dirs;
}

// Directory of the source being split. A quote include resolves relative to the including
// file first, so this behaves as an implicit include directory: without it a header sitting
// beside its source falls to the _abs fallback below, and its rewritten copy is then
// unreachable at the path the source spells -- the original wins the lookup and its
// definitions collide with the split pieces that also define them.
static std::string g_unit_source_dir;

static std::vector<std::string> unit_include_dirs(const std::vector<std::string>& flags) {
    std::vector<std::string> dirs = include_dirs_from_flags(flags);
    if (!g_unit_source_dir.empty()) {
        if (std::find(dirs.begin(), dirs.end(), g_unit_source_dir) == dirs.end())
            dirs.push_back(g_unit_source_dir);
        std::sort(dirs.begin(), dirs.end(),
                  [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    }
    return dirs;
}

static std::string path_hash(const std::string& s) {
    size_t h = std::hash<std::string>{}(s);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016zx", h);
    return std::string(buf);
}

// Where a header's rewritten copy lives, relative to <split_dir>/include: the path it was
// included as. A header under no include directory falls back to a hash of its absolute
// path, which keeps the mapping unique rather than colliding on the basename.
static std::string header_mirror_relpath(const std::string& abs_header,
                                         const std::vector<std::string>& include_dirs) {
    fs::path h = fs::path(abs_header).lexically_normal();
    for (const auto& d : include_dirs) {
        fs::path rel = h.lexically_relative(fs::path(d));
        if (rel.empty()) continue;
        std::string r = rel.string();
        if (r.rfind("..", 0) == 0) continue;   // not underneath this directory
        return r;
    }
    return (fs::path("_abs") / path_hash(abs_header) / h.filename()).string();
}

static std::string split_include_root(const std::string& split_dir) {
    return (fs::path(split_dir) / "include").string();
}

// Guards against two different sources being written to one output path. A collision
// means the layout mapping has a gap, and silently overwriting would substitute one
// header for another instead of reporting a problem.
static std::unordered_map<std::string, std::string> g_claimed_outputs;

static bool claim_output_path(const std::string& out_path, const std::string& src_abs) {
    auto it = g_claimed_outputs.find(out_path);
    if (it != g_claimed_outputs.end() && it->second != src_abs) {
        std::cerr << "cpp-splitter: split output path collision: " << out_path
                  << "\n  already claimed by: " << it->second
                  << "\n  also claimed by:    " << src_abs << "\n";
        return false;
    }
    g_claimed_outputs[out_path] = src_abs;
    return true;
}

static SplitResult do_split(const std::string& input_path,
                            const std::string& output_dir,
                            const std::vector<std::string>& extra_flags,
                            bool verbose,
                            std::ostream& out = std::cout);

static bool auto_include_split_enabled() {
    const char* val = std::getenv("CPP_SPLITTER_AUTO_INCLUDE_SPLIT");
    if (val) {
        std::string s(val);
        if (s == "off" || s == "0") return false;
    }
    return true;
}

// --- Headers that must not be split --------------------------------------------------
//
// Automatic header splitting parses each discovered header as its own translation unit.
// Many real headers are not self-contained by design -- prefix/suffix and push/pop pairs,
// and headers that need a macro context their includer establishes -- and cannot be parsed
// alone. A header that fails to parse must be left alone entirely rather than split from a
// partial AST, because the rewritten copy goes on the include path ahead of the real one:
// a half-parsed copy would silently displace a correct header.

// A header meant to be included once carries an include guard. One deliberately designed
// for repeated inclusion -- one half of a header/footer or push/pop pair -- carries none,
// and rewriting either half on its own is never correct.
static bool header_has_include_guard(const std::string& source) {
    const std::string blanked = blank_code_noise(source);
    if (blanked.find("#pragma once") != std::string::npos ||
        blanked.find("# pragma once") != std::string::npos)
        return true;

    // Classic guard: `#ifndef X` followed within a few lines by `#define X`.
    std::istringstream iss(blanked);
    std::string line, guard;
    int since_ifndef = -1;
    while (std::getline(iss, line)) {
        std::istringstream ls(line);
        std::string hash, directive, ident;
        ls >> hash;
        if (hash == "#") {
            ls >> directive >> ident;
        } else if (!hash.empty() && hash[0] == '#') {
            directive = hash.substr(1);
            ls >> ident;
        } else {
            if (since_ifndef >= 0 && ++since_ifndef > 4) since_ifndef = -1;
            continue;
        }
        if (directive == "ifndef" && !ident.empty()) {
            guard = ident;
            since_ifndef = 0;
        } else if (directive == "define" && since_ifndef >= 0 && ident == guard) {
            return true;
        } else if (since_ifndef >= 0 && ++since_ifndef > 4) {
            since_ifndef = -1;
        }
    }
    return false;
}

// Cache the decision not to split a header, so it is made once rather than on every
// invocation. An empty compilable list is what load_header_manifests() treats as "nothing
// to link", and resolve_header_deps() takes the file's existence as "already decided".
static void write_skipped_header_manifest(const std::string& unit_dir,
                                          const std::string& header_filename,
                                          const std::string& abs_header_path,
                                          const std::string& reason) {
    std::error_code ec;
    fs::create_directories(unit_dir, ec);
    if (ec) return;
    std::ofstream ofs((fs::path(unit_dir) / (header_filename + ".split")).string());
    if (!ofs.is_open()) return;
    ofs << abs_header_path << "\n" << 0 << "\n";
    ofs << "# not split: " << reason << "\n";
}

static void emit_split_files(CXTranslationUnit tu,
                             const std::vector<FunctionInfo>& functions,
                             const std::vector<VariableInfo>& variables,
                             const std::string& input_path,
                             const std::string& abs_path,
                             const std::string& output_dir,
                             const std::string& include_root,
                             const std::string& unit_tag,
                             const std::string& preamble_filename,
                             const std::string& preamble_path,
                             const std::string& definitions_filename,
                             const std::vector<std::string>& all_flags,
                             const std::vector<std::string>& extra_flags,
                             const std::string& context_preamble,
                             bool input_is_header,
                             bool parse_clean,
                             bool verbose,
                             std::ostream& out,
                             SplitResult& result);

// Headers this translation unit should split. Decided before the AST walk so that the walk
// records only functions that will actually be used.
static void register_header_manifest(const fs::path& manifest_path,
                                     const std::string& include_root);

static std::vector<std::string> header_split_candidates(
        CXTranslationUnit tu,
        const std::string& main_file,
        const std::string& output_dir,
        const std::vector<std::string>& extra_flags,
        bool verbose,
        std::ostream& out) {
    std::vector<std::string> candidates;
    if (!auto_include_split_enabled())
        return candidates;

    std::vector<std::string> includes;
    clang_getInclusions(tu, inclusion_visitor, &includes);

    const auto inc_dirs = unit_include_dirs(extra_flags);
    const std::string include_root = split_include_root(output_dir);

    // How many times each file was pulled in. A file with no include guard that is read
    // repeatedly is one half of a header/footer or push/pop pair; one read exactly once is
    // an implementation include, which is a perfectly ordinary thing to split.
    std::map<std::string, int> inclusion_count;
    for (const auto& inc_path : includes)
        ++inclusion_count[inc_path];

    std::set<std::string> seen;
    for (const auto& inc_path : includes) {
        if (!seen.insert(inc_path).second) continue;
        // clang_getInclusions() reports the file being compiled among its own inclusions.
        if (inc_path == main_file) continue;
        // Deliberately not filtered by extension: what makes a file a header here is that
        // the translation unit included it, which is exactly what clang_getInclusions()
        // reports. An implementation include -- .ipp, .inc, .inl, .tcc -- is as splittable
        // as anything else, and Boost.Filesystem puts a whole translation unit's worth of
        // code in one.
        if (is_stdlib_header(inc_path)) continue;
        if (g_split_headers.find(inc_path) != g_split_headers.end()) continue;

        const std::string rel = header_mirror_relpath(inc_path, inc_dirs);
        const std::string manifest = (fs::path(include_root) / (rel + ".split")).string();
        bool stale = false;
        if (fs::exists(manifest)) {
            std::error_code ec;
            auto hdr_time = fs::last_write_time(inc_path, ec);
            auto man_time = fs::last_write_time(manifest, ec);
            if (ec) continue;
            stale = (hdr_time > man_time);
        }
        if (fs::exists(manifest) && !stale) {
            // Already split by an earlier run of this same object. There is nothing to
            // re-split, but its pieces still have to be compiled into *this* object, and
            // resolve_header_deps() finds them only through g_split_headers. Skipping
            // without registering dropped every definition the header contributed, silently:
            // the unit reported success and those symbols were in no object at all.
            //
            // This has to happen here rather than by pre-loading every manifest up front,
            // because the g_split_headers test above runs before the staleness test -- a
            // pre-loaded stale header would be skipped rather than re-split, and header edits
            // would stop being detected.
            register_header_manifest(manifest, include_root);
            continue;
        }

        // A file with no include guard read more than once is one half of a pair, and
        // rewriting half a pair is never correct. Read exactly once it is an implementation
        // include, which has no guard for the ordinary reason that it does not need one.
        const std::string src = read_file(inc_path);
        if (src.empty() ||
            (!header_has_include_guard(src) && inclusion_count[inc_path] > 1)) {
            if (verbose)
                out << "Skipping " << inc_path << ": no include guard and included "
                    << inclusion_count[inc_path] << " times, so it is one half of a pair"
                       " and is not meant to be included on its own\n";
            const std::string unit_dir =
                (fs::path(include_root) / fs::path(rel).parent_path()).string();
            write_skipped_header_manifest(unit_dir, fs::path(inc_path).filename().string(),
                                          inc_path, "no include guard, included repeatedly");
            continue;
        }
        candidates.push_back(inc_path);
    }
    return candidates;
}

// Writes the preamble and the split files for one file -- the translation unit itself or
// one of its headers -- from an inventory already harvested out of `tu`.
static SplitResult split_unit(CXTranslationUnit tu,
                              const std::string& abs_path,
                              const std::string& input_path,
                              const std::string& source,
                              std::vector<FunctionInfo>& functions,
                              std::vector<VariableInfo>& variables,
                              const std::set<std::string>& referenced,
                              // Whether this unit is being split as an included file rather
                              // than as the file being compiled. The caller knows; the file
                              // name does not, and an implementation include is a header
                              // here whatever it is called.
                              bool input_is_header,
                              const std::string& output_dir,
                              const std::vector<std::string>& all_flags,
                              const std::vector<std::string>& extra_flags,
                              const std::string& context_preamble,
                              bool parse_clean,
                              bool verbose,
                              std::ostream& out) {
    SplitResult result;
    result.success = false;

    const std::string preamble_filename = input_is_header
        ? fs::path(abs_path).filename().string()
        : fs::path(abs_path).stem().string() + "_preamble.h";

    std::string unit_dir = output_dir;
    if (input_is_header) {
        const std::string rel =
            header_mirror_relpath(abs_path, unit_include_dirs(extra_flags));
        unit_dir = (fs::path(split_include_root(output_dir)) /
                    fs::path(rel).parent_path()).string();
    }

    const std::string unit_tag = sanitize_filename(fs::path(abs_path).filename().string());
    const std::string preamble_path = (fs::path(unit_dir) / preamble_filename).string();

    prepare_functions(functions, source, referenced, input_is_header);
    prepare_variables(variables, functions, source, unit_tag, input_is_header);
    dump_keep_decisions(functions);

    // A translation unit with nothing of its own to split still needs its preamble on disk:
    // the pieces split out of its headers include it to compile in the context the header
    // was seen in, and a unit whose whole body arrives through an included file has no
    // functions of its own at all.
    const bool nothing_of_its_own = functions.empty() && variables.empty();
    if (nothing_of_its_own) {
        if (verbose) out << "No function definitions found in " << input_path << "\n";
        if (input_is_header) {
            result.success = true;
            return result;
        }
    }

    if (!claim_output_path(preamble_path, abs_path))
        return result;

    fs::create_directories(unit_dir);

    result.preamble_filename = preamble_path;
    const StaticRenameMap static_renames =
        build_static_rename_map(functions, variables, unit_tag);
    std::string definitions;
    const std::string preamble =
        generate_preamble(source, functions, variables, unit_tag, abs_path, static_renames,
                          &definitions);

    std::string existing_preamble;
    if (fs::exists(preamble_path))
        existing_preamble = read_file(preamble_path);
    if (existing_preamble != preamble) {
        std::ofstream ofs(preamble_path);
        if (!ofs.is_open()) {
            std::cerr << "Error: cannot write preamble to " << preamble_path << "\n";
            return result;
        }
        ofs << preamble;
        ofs.close();
        if (verbose) out << "Generated preamble: " << preamble_path << " (updated)\n";
    } else {
        if (verbose) out << "Preamble unchanged: " << preamble_path << "\n";
    }

    // Definitions that may exist in only one object go to a second header, included by
    // exactly one piece. The preamble keeps a declaration in their place.
    std::string definitions_filename;
    if (!definitions.empty()) {
        definitions_filename = unit_tag + "_definitions.h";
        const std::string definitions_path =
            (fs::path(unit_dir) / definitions_filename).string();
        const std::string body =
            "#pragma once\n#include \"" + preamble_filename + "\"\n\n" + definitions;
        if (!fs::exists(definitions_path) || read_file(definitions_path) != body) {
            std::ofstream ofs(definitions_path);
            if (!ofs.is_open()) {
                std::cerr << "Error: cannot write definitions to " << definitions_path << "\n";
                return result;
            }
            ofs << body;
            ofs.close();
            if (verbose)
                out << "Generated definitions header: " << definitions_path << "\n";
        }
    }

    if (nothing_of_its_own) {
        result.success = true;
        return result;
    }

    emit_split_files(tu, functions, variables, input_path, abs_path, unit_dir,
                     split_include_root(output_dir), unit_tag,
                     preamble_filename, preamble_path, definitions_filename,
                     all_flags, extra_flags,
                     context_preamble, input_is_header, parse_clean, verbose, out, result);
    result.success = true;
    return result;
}

// Splits every header this translation unit should split, using the inventory harvested
// from `tu` rather than re-parsing each header on its own, then collects the objects and
// include directories the caller has to link and compile against.
static void resolve_header_deps(CXTranslationUnit tu,
                                const HarvestMap& harvest,
                                const VarHarvestMap& var_harvest,
                                const std::set<std::string>& referenced,
                                const std::vector<std::string>& candidates,
                                const std::string& context_preamble,
                                SplitResult& result,
                                const std::string& output_dir,
                                const std::vector<std::string>& all_flags,
                                const std::vector<std::string>& extra_flags,
                                bool parse_clean,
                                bool verbose,
                                std::ostream& out) {
    for (const auto& inc_path : candidates) {
        if (verbose) out << "\n[auto-split] " << inc_path << "\n";

        auto it = harvest.find(inc_path);
        std::vector<FunctionInfo> fns =
            (it == harvest.end()) ? std::vector<FunctionInfo>() : it->second;
        auto vit = var_harvest.find(inc_path);
        std::vector<VariableInfo> vars =
            (vit == var_harvest.end()) ? std::vector<VariableInfo>() : vit->second;

        const std::string src = read_file(inc_path);
        if (src.empty()) continue;

        if (fns.empty()) {
            // Nothing to split, but record the decision so it is not reconsidered on every
            // invocation.
            const std::string rel =
                header_mirror_relpath(inc_path, unit_include_dirs(extra_flags));
            const std::string unit_dir =
                (fs::path(split_include_root(output_dir)) / fs::path(rel).parent_path()).string();
            if (verbose) out << "No function definitions found in " << inc_path << "\n";
            write_skipped_header_manifest(unit_dir, fs::path(inc_path).filename().string(),
                                          inc_path, "no function definitions");
            continue;
        }

        SplitResult hdr_sr = split_unit(tu, inc_path, inc_path, src, fns, vars, referenced, true,
                                        output_dir, all_flags, extra_flags, context_preamble,
                                        parse_clean, verbose, out);
        if (!hdr_sr.success && verbose)
            out << "[auto-split] warning: failed to split " << inc_path << "\n";
    }

    std::vector<std::string> includes;
    clang_getInclusions(tu, inclusion_visitor, &includes);

    std::set<std::string> seen_dirs;
    for (const auto& inc_path : includes) {
        auto it = g_split_headers.find(inc_path);
        if (it != g_split_headers.end()) {
            if (verbose) out << "[header-dep] " << inc_path << " -> " << it->second.split_dir << "\n";
            if (seen_dirs.insert(it->second.split_dir).second)
                result.header_obj_dirs.push_back(it->second.split_dir);
            for (const auto& cpp : it->second.compilable_files) {
                std::string obj = cpp.substr(0, cpp.size() - 4) + ".o";
                result.header_obj_files.push_back(obj);
            }
        }
    }
}

static void write_header_manifest(const std::string& output_dir,
                                   const std::string& header_filename,
                                   const std::string& abs_header_path,
                                   const std::vector<std::string>& compilable_files) {
    std::string manifest_path = (fs::path(output_dir) / (header_filename + ".split")).string();
    std::ofstream ofs(manifest_path);
    if (!ofs.is_open()) return;
    ofs << abs_header_path << "\n";
    ofs << compilable_files.size() << "\n";
    for (const auto& f : compilable_files) ofs << f << "\n";
}

// One manifest: the header it was generated from, then the pieces it produced.
static void register_header_manifest(const fs::path& manifest_path,
                                     const std::string& include_root) {
    const std::string fname = manifest_path.filename().string();
    if (fname.size() < 6 || fname.substr(fname.size() - 6) != ".split") return;

    std::ifstream ifs(manifest_path);
    if (!ifs.is_open()) return;

    std::string abs_path;
    std::getline(ifs, abs_path);
    std::string n;
    std::getline(ifs, n);
    int nfiles = 0;
    try { nfiles = std::stoi(n); } catch (...) {}

    SplitHeaderInfo info;
    info.split_dir = include_root;
    info.preamble_path =
        (manifest_path.parent_path() / fname.substr(0, fname.size() - 6)).string();
    for (int i = 0; i < nfiles; ++i) {
        std::string f;
        std::getline(ifs, f);
        if (!f.empty()) info.compilable_files.push_back(f);
    }

    // An empty list is how a header that was examined and deliberately not split is recorded.
    if (!abs_path.empty() && !info.compilable_files.empty())
        g_split_headers[abs_path] = std::move(info);
}

static void load_header_manifests(const std::string& output_dir) {
    if (!fs::exists(output_dir)) return;
    const std::string include_root = split_include_root(output_dir);
    // Manifests are nested under the mirrored include tree, so this has to recurse.
    for (const auto& entry : fs::recursive_directory_iterator(output_dir)) {
        if (entry.is_regular_file())
            register_header_manifest(entry.path(), include_root);
    }
}


// The compiler this run will hand the split pieces to: the one the launcher was asked to
// wrap, or the one `--cxx` names. Empty only when neither said, in which case the probes
// below fall back to a default.
static std::string g_compiler;

// The system include directories to hand libclang.
//
// They must come from the compiler this build actually uses, not from whatever `g++` happens
// to be on PATH. The two are not interchangeable: GCC's `xmmintrin.h` implements the SSE
// intrinsics with `__builtin_ia32_*`, which clang does not have, so feeding GCC's include
// directories to libclang made every parse that reached `<xmmintrin.h>` fail --
//
//     xmmintrin.h:136: error: use of undeclared identifier '__builtin_ia32_addss'
//
// -- and the unit fall back. Nothing in Boost reached it until Boost.Geometry, which pulls it
// in through Boost.Multiprecision; then it accounted for most of Geometry's test suite.
static const std::vector<std::string>& cached_system_includes() {
    static std::vector<std::string> includes =
        g_compiler.empty() ? detect_system_includes() : detect_system_includes(g_compiler);
    return includes;
}

// The language standard the *driver* would use for this compile, as a flag to hand libclang.
//
// When the command line carries no -std, the driver and libclang each fall back to their own
// default, and they need not agree: clang 13's driver defaults to gnu++14 while libclang,
// given the same arguments, parses at C++17. The splitter then decides what to split by
// reading one program and compiles a different one -- every #if in the source is evaluated
// twice against two macro environments. It surfaced as five unrelated-looking failures: two
// redefinitions of main, a missing std::pmr, an undeclared identifier and three multiple
// definitions.
//
// The driver's answer is the reference: it is what the build system asked for, and it is
// what compiles the pieces and the fallback object. So ask it.
static std::string probe_driver_standard(const std::string& compiler) {
    std::string cmd = shell_quote(compiler) + " -x c++ -E -dM /dev/null 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";

    char buf[4096];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    pclose(pipe);

    std::string value;
    bool strict_ansi = false;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("#define __cplusplus ", 0) == 0)
            value = trim_ws(line.substr(std::strlen("#define __cplusplus ")));
        else if (line.rfind("#define __STRICT_ANSI__", 0) == 0)
            strict_ansi = true;
    }
    while (!value.empty() && (value.back() == 'L' || value.back() == 'l')) value.pop_back();

    // The GNU dialects differ from the ISO ones in more than __cplusplus, so keep whichever
    // family the driver is in rather than normalising to -std=c++NN.
    const std::string dialect = strict_ansi ? "c++" : "gnu++";
    static const std::pair<const char*, const char*> known[] = {
        {"199711", "98"}, {"201103", "11"}, {"201402", "14"},
        {"201703", "17"}, {"202002", "20"}, {"202302", "23"},
    };
    for (const auto& k : known)
        if (value == k.first) return "-std=" + dialect + k.second;
    return "";
}

// The standard to parse at when the caller has already fixed one for the compile. The
// command-line driver compiles its pieces with -std=c++17 written into the command it
// builds, so probing the driver's *default* would make the parse disagree with the compile
// -- which is the bug TODO/19 was about, mirrored.
static std::string g_forced_standard;

static const std::string& cached_driver_standard() {
    static const std::string std_flag =
        g_compiler.empty() ? std::string() : probe_driver_standard(g_compiler);
    return std_flag;
}

static std::vector<std::string> build_clang_flags(const std::vector<std::string>& extra_flags,
                                                   bool force_cxx = false) {
    // The command line wins: if it names a standard, both sides already agree.
    bool have_std = false;
    for (const auto& f : extra_flags)
        if (f.rfind("-std=", 0) == 0 || f == "--std") { have_std = true; break; }

    std::string std_flag;
    if (!have_std)
        std_flag = g_forced_standard.empty() ? cached_driver_standard() : g_forced_standard;

    // Whichever of the three won, that is the standard the pieces are compiled at, so it is
    // the one the splitter's own decisions have to be made against.
    if (have_std) {
        for (const auto& f : extra_flags)
            if (f.rfind("-std=", 0) == 0) { g_cxx_standard = standard_from_flag(f); break; }
    } else {
        g_cxx_standard = standard_from_flag(std_flag);
    }
    // Nothing to probe, or the driver said something unrecognised. Falling back to a fixed
    // standard is what this used to do unconditionally, and it is better than leaving
    // libclang to a default that varies with how libclang itself was built.
    if (!have_std && std_flag.empty()) std_flag = "-std=c++17";

    std::vector<std::string> all_flags = {"-fsyntax-only", "-Wno-everything"};
    if (!std_flag.empty()) all_flags.insert(all_flags.begin(), std_flag);
    if (force_cxx)
        all_flags.insert(all_flags.begin(), {"-x", "c++-header"});
    const auto& sys_includes = cached_system_includes();
    for (const auto& inc : sys_includes) {
        all_flags.push_back("-isystem");
        all_flags.push_back(inc);
    }
    for (const auto& f : extra_flags)
        all_flags.push_back(f);
    return all_flags;
}

static unsigned check_diagnostics(CXTranslationUnit tu, bool verbose) {
    unsigned num_diag = clang_getNumDiagnostics(tu);
    unsigned error_count = 0;
    for (unsigned di = 0; di < num_diag; ++di) {
        CXDiagnostic diag = clang_getDiagnostic(tu, di);
        CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diag);
        if (sev >= CXDiagnostic_Error) {
            if (verbose)
                std::cerr << "Parse error: "
                          << cx_to_string(clang_formatDiagnostic(diag, CXDiagnostic_DisplaySourceLocation))
                          << "\n";
            ++error_count;
        }
        clang_disposeDiagnostic(diag);
    }
    return error_count;
}

// Shared tail of do_split(): builds the PCH, writes one split
// .cpp per function, prunes outputs left over from a previous run, and registers header
// dependencies. Both callers reach this point with identical state; keeping it in one
// place is what stops the two paths from drifting apart.
static void emit_split_files(CXTranslationUnit tu,
                             const std::vector<FunctionInfo>& functions,
                             const std::vector<VariableInfo>& variables,
                             const std::string& input_path,
                             const std::string& abs_path,
                             const std::string& output_dir,
                             const std::string& include_root,
                             const std::string& unit_tag,
                             const std::string& preamble_filename,
                             const std::string& preamble_path,
                             const std::string& definitions_filename,
                             const std::vector<std::string>& all_flags,
                             const std::vector<std::string>& extra_flags,
                             const std::string& context_preamble,
                             bool input_is_header,
                             bool parse_clean,
                             bool verbose,
                             std::ostream& out,
                             SplitResult& result) {
    // The preamble is no longer precompiled. Its PCH had no consumer -- TODO 08 removed the
    // one use of it, because feeding a preamble's precompiled form back into a parse of the
    // source it was derived from redefines everything the source declares. The include
    // prefix is precompiled instead, which is where the parse time actually is.

    if (verbose) out << "Found " << functions.size() << " function(s) in " << input_path << ":\n\n";

    // The same map generate_preamble() used. It has to be the same, or a piece refers to a
    // name the preamble no longer declares.
    const StaticRenameMap static_renames =
        build_static_rename_map(functions, variables, unit_tag);

    bool definitions_emitted = false;
    std::vector<std::string> current_files;

    // Why each definition that could not be split was kept. One file per unit rather than a
    // note at the top of eleven thousand pieces that are never compiled.
    std::ostringstream keeps;
    int file_counter = 0;
    int written_count = 0;
    int skipped_count = 0;
    for (const auto& fn : functions) {
        ++file_counter;

        std::string safe_name = sanitize_filename(fn.name);
        if (safe_name.empty()) safe_name = "anonymous";

        std::string out_filename = unit_tag + "_" +
                                   std::to_string(file_counter) + "_" +
                                   safe_name + ".cpp";
        std::string out_path = (fs::path(output_dir) / out_filename).string();
        current_files.push_back(out_path);

        std::ostringstream content;
        content << "// Function: " << fn.signature << "\n";
        content << "// Source: " << input_path << " (lines " << fn.start_line << "-" << fn.end_line << ")\n";
        if (should_keep_in_header(fn))
            content << "// Note: kept in the preamble, not compiled -- "
                    << keep_reason(fn) << "\n";
        content << "// ---\n\n";
        // A header is not necessarily self-contained: it is written to be included at a
        // particular point, after earlier includes have completed the types it uses.
        // Including its rewritten copy on its own reimposes a self-containedness
        // requirement the original never had. Including the translation unit's preamble
        // first replays the include prefix the header was actually seen behind.
        if (!context_preamble.empty())
            content << "#include \"" << context_preamble << "\"\n";
        content << "#include \"" << preamble_filename << "\"\n";
        // The definitions header carries what may exist in only one object, so exactly one
        // piece includes it. Which one does not matter; the first compilable one will do.
        if (!definitions_filename.empty() && !definitions_emitted && !should_keep_in_header(fn)) {
            content << "#include \"" << definitions_filename << "\"\n";
            definitions_emitted = true;
        }
        content << "\n";

        // Members are emitted in their out-of-line form; everything else as written. An
        // out-of-line form was rebuilt from the declarator and no longer carries the
        // attribute, so only the verbatim body needs stripping.
        std::string body = fn.outlined_body.empty()
                               ? strip_always_inline(fn.body, fn.start_offset,
                                                     fn.always_inline_ranges)
                               : fn.outlined_body;
        if (fn.is_static) {
            // Best effort by design: `is_static` reflects linkage, not syntax. A
            // function in an anonymous namespace, or an `inline` one at namespace
            // scope, has internal linkage and no `static` keyword to remove -- the
            // text must then be left exactly as it is.
            body = strip_decl_specifier(body, "static");
        }
        // Linkage of the split-out definition.
        //
        // A definition taken out of a .cpp is the only one there will ever be, so `inline`
        // is dropped: an inline function that its own translation unit never odr-uses is
        // not emitted at all, and the declaration in the preamble promises an ordinary
        // symbol. Leaving it produced objects with dangling references.
        //
        // A definition taken out of a *header* is different: the header can be included by
        // many translation units, each with its own split directory, so several objects
        // will carry the same function. `inline` is exactly what makes that legal -- vague
        // linkage lets the linker merge the copies -- so it stays, and instead the symbol
        // is forced into existence. `-fkeep-inline-functions` used to be relied on for
        // that; it is a GCC option that clang parses and ignores, so it never worked here.
        if (!input_is_header) {
            body = strip_decl_specifier(body, "inline");
        } else {
            // A member function defined inside its class body is implicitly inline. Writing
            // it out-of-line loses that, so every translation unit including the header
            // emits a strong definition and the final link fails with multiple definition.
            // Say `inline` explicitly whenever the text does not already. This has to run
            // before the #line directive is prepended, or it lands in front of it.
            const std::string blanked = blank_code_noise(body);
            const size_t at = inline_insertion_point(blanked);
            if (at != std::string::npos &&
                !contains_decl_token(blanked.substr(0, decl_prefix_end(blanked)), "inline"))
                body.insert(at, at == 0 ? "inline " : " inline");
        }

        body = apply_static_renames(body, static_renames);

        std::string line_directive = "#line " + std::to_string(fn.start_line) +
                                     " \"" + abs_path + "\"\n";

        body = line_directive + body;
        if (input_is_header)
            body = "__attribute__((used))\n" + body;

        for (const auto& c : fn.conditionals)
            content << c << "\n";

        if (!fn.scope_chain.empty()) {
            content << wrap_in_namespaces(body, fn.scope_chain) << "\n";
        } else {
            content << body << "\n";
        }

        for (size_t ci = 0; ci < fn.conditionals.size(); ++ci)
            content << "#endif\n";

        std::string new_content = content.str();
        const bool kept = should_keep_in_header(fn);

        // A kept definition's piece is never compiled -- there is no object to be had from a
        // function template, and nothing to gain from one. Writing it anyway cost 97.6% of
        // the files this produces on Boost.Geometry: 11515 written against 273 compiled for
        // one translation unit, 12 MB of .cpp that nothing reads. TODO 27.
        //
        // The counter still advances, so the names of the pieces that *are* compiled do not
        // move and an incremental build does not see everything change. The reason the
        // definition was kept is recorded once per unit in <tag>.keeps instead of once per
        // definition in a file of its own, and CPP_SPLITTER_DUMP_HARVEST brings the old
        // per-file form back when that is what the question needs.
        if (kept && !std::getenv("CPP_SPLITTER_DUMP_HARVEST")) {
            keeps << file_counter << "\t" << fn.start_line << "\t" << keep_reason(fn)
                  << "\t" << fn.signature << "\n";
            current_files.pop_back();   // not written, so not ours to keep from the pruner
            if (verbose) {
                out << "  [" << file_counter << "] " << fn.signature << "  (header-only)\n";
                out << "      Lines " << fn.start_line << "-" << fn.end_line
                    << " -> kept in the preamble, no piece written\n";
            }
            continue;
        }

        bool needs_write = true;
        if (fs::exists(out_path)) {
            std::string existing = read_file(out_path);
            if (existing == new_content)
                needs_write = false;
        }

        if (needs_write) {
            std::ofstream ofs(out_path);
            if (!ofs.is_open()) {
                std::cerr << "Error: cannot write to " << out_path << "\n";
                continue;
            }
            ofs << new_content;
            ofs.close();
            ++written_count;
        } else {
            ++skipped_count;
        }

        if (!kept)
            result.compilable_files.push_back(out_path);

        if (verbose) {
            out << "  [" << file_counter << "] " << fn.signature;
            if (kept) out << "  (header-only)";
            if (!needs_write) out << "  (unchanged)";
            out << "\n";
            out << "      Lines " << fn.start_line << "-" << fn.end_line
                << " -> " << out_path << "\n";
        }
    }

    {
        const std::string keeps_path = (fs::path(output_dir) / (unit_tag + ".keeps")).string();
        const std::string keeps_text = keeps.str();
        if (keeps_text.empty()) {
            std::error_code ec;
            fs::remove(keeps_path, ec);
        } else if (!fs::exists(keeps_path) || read_file(keeps_path) != keeps_text) {
            std::ofstream ofs(keeps_path);
            if (ofs.is_open()) ofs << keeps_text;
        }
    }

    // Every definition was kept in the preamble, so no piece was there to include the
    // definitions header -- and what it holds is precisely what may exist in only one
    // object, which would then be no object at all. Give it a piece of its own.
    // Boost.Serialization's xml_grammar.cpp is a whole translation unit whose only
    // definition is one explicit specialization: nothing was compiled, and every archive
    // that linked against it was left without basic_xml_grammar<char>::init_chset().
    if (!definitions_filename.empty() && !definitions_emitted) {
        const std::string out_filename = unit_tag + "_0_definitions.cpp";
        const std::string out_path = (fs::path(output_dir) / out_filename).string();
        std::ostringstream content;
        content << "// Definitions kept out of the preamble because they may exist in only\n"
                << "// one object. No split piece was compiled to carry them.\n";
        content << "// Source: " << input_path << "\n";
        content << "// ---\n\n";
        if (!context_preamble.empty())
            content << "#include \"" << context_preamble << "\"\n";
        content << "#include \"" << definitions_filename << "\"\n";
        const std::string new_content = content.str();
        if (!fs::exists(out_path) || read_file(out_path) != new_content) {
            std::ofstream ofs(out_path);
            if (ofs.is_open()) ofs << new_content;
        }
        current_files.push_back(out_path);
        result.compilable_files.push_back(out_path);
        if (verbose)
            out << "  [definitions] -> " << out_path << "\n";
    }

    // Pruning removes outputs that the current run did not produce. After a parse that
    // reported errors the current file list is not trustworthy -- a run that saw almost no
    // functions would delete a good run's work -- so leave the directory alone.
    int removed_count = 0;
    for (const auto& entry : parse_clean ? fs::directory_iterator(output_dir)
                                         : fs::directory_iterator()) {
        if (!entry.is_regular_file()) continue;
        std::string path = entry.path().string();
        std::string fname = entry.path().filename().string();
        if (fname == preamble_filename) continue;
        if (fname.size() < 4 || fname.substr(fname.size() - 4) != ".cpp") continue;
        if (fname.rfind(unit_tag + "_", 0) != 0) continue;
        if (std::find(current_files.begin(), current_files.end(), path) == current_files.end()) {
            fs::remove(entry.path());
            fs::path obj_path = entry.path();
            obj_path.replace_extension(".o");
            if (fs::exists(obj_path))
                fs::remove(obj_path);
            if (verbose) out << "  Removed stale: " << fname << "\n";
            ++removed_count;
        }
    }

    if (verbose) {
        out << "\n" << file_counter << " function(s): "
            << written_count << " written, "
            << skipped_count << " unchanged";
        if (removed_count > 0)
            out << ", " << removed_count << " stale removed";
        out << "\n";
    }

    if (input_is_header) {
        SplitHeaderInfo hdr_info;
        hdr_info.split_dir = fs::absolute(include_root).string();
        hdr_info.preamble_path = preamble_path;
        hdr_info.compilable_files = result.compilable_files;
        g_split_headers[abs_path] = std::move(hdr_info);
        write_header_manifest(output_dir, preamble_filename, abs_path, result.compilable_files);
        if (verbose) out << "Registered split header: " << abs_path << " (" << result.compilable_files.size() << " compilable files)\n";
    }
}



static SplitResult do_split(const std::string& input_path,
                            const std::string& output_dir,
                            const std::vector<std::string>& extra_flags,
                            bool verbose,
                            std::ostream& out) {
    SplitResult result;
    result.success = false;

    std::string abs_path = fs::absolute(input_path).string();
    std::string source = read_file(abs_path);
    if (source.empty()) {
        if (verbose) std::cerr << "Error: could not read file or file is empty\n";
        return result;
    }

    std::string stem = fs::path(input_path).stem().string();
    bool input_is_header = is_header_file(abs_path);
    auto all_flags = build_clang_flags(extra_flags, input_is_header);

    std::string preamble_filename = input_is_header
        ? fs::path(input_path).filename().string()
        : stem + "_preamble.h";

    // A header is mirrored under <split_dir>/include at the path it was included as; the
    // translation unit itself keeps the root of the split directory.
    std::string unit_dir = output_dir;
    if (input_is_header) {
        std::string rel = header_mirror_relpath(abs_path, unit_include_dirs(extra_flags));
        unit_dir = (fs::path(split_include_root(output_dir)) /
                    fs::path(rel).parent_path()).string();
    }

    // Split pieces are named after the whole file name rather than its stem, so a source
    // and a header that share a stem (path.cpp / path.hpp) can never match each other's
    // outputs -- the stale-output pruning keys off this prefix.
    std::string unit_tag = sanitize_filename(fs::path(abs_path).filename().string());
    std::string preamble_path = (fs::path(unit_dir) / preamble_filename).string();

    if (input_is_header && !header_has_include_guard(source)) {
        if (verbose)
            out << "Skipping " << input_path
                << ": no include guard, so it is one half of a pair and is not meant to be"
                   " included on its own\n";
        write_skipped_header_manifest(unit_dir, preamble_filename, abs_path,
                                      "no include guard");
        result.success = true;
        return result;
    }

    // The preamble is this very file with the function bodies carved out, so it declares
    // every class, enum and typedef the file declares. Feeding its PCH back in while
    // parsing the file redefines all of them. The first run got away with it because no
    // preamble existed yet; from the second run on the parse was wrecked, the visitor
    // found almost nothing, and the stale-output pruning then deleted the previous run's
    // split files. The PCH is for compiling the split pieces, which include the preamble
    // and not the original source -- it must not be applied here.
    //
    // libclang's own preamble caching is a different mechanism and is safe: it caches the
    // prefix of *this* file, so it cannot redefine anything.
    std::vector<std::string> parse_flags_vec = all_flags;

    // Precompile the include prefix and hand it back, so the include graph is parsed once
    // per distinct prefix rather than once per split.
    {
        const std::string prefix = include_prefix_of(source);
        if (!prefix.empty()) {
            std::error_code ec;
            fs::create_directories(unit_dir, ec);
            const std::string prefix_path =
                (fs::path(unit_dir) / (unit_tag + "_prefix.h")).string();
            if (!fs::exists(prefix_path) || read_file(prefix_path) != prefix) {
                std::ofstream ofs(prefix_path);
                if (ofs.is_open()) ofs << prefix;
            }
            const std::string prefix_pch =
                build_libclang_pch(prefix_path, all_flags, verbose, out);
            if (!prefix_pch.empty()) {
                parse_flags_vec.push_back("-include-pch");
                parse_flags_vec.push_back(prefix_pch);
            }
        }
    }

    std::vector<const char*> args;
    for (const auto& f : parse_flags_vec)
        args.push_back(f.c_str());

    CXIndex index = clang_createIndex(0, 0);
    if (!index) {
        if (verbose) std::cerr << "Error: failed to create clang index\n";
        return result;
    }

    CXTranslationUnit tu = nullptr;
    unsigned parse_flags = CXTranslationUnit_PrecompiledPreamble
                         | CXTranslationUnit_CreatePreambleOnFirstParse;
    CXErrorCode err = clang_parseTranslationUnit2(
        index, abs_path.c_str(), args.data(),
        static_cast<int>(args.size()), nullptr, 0, parse_flags, &tu);

    if (err != CXError_Success || !tu) {
        if (verbose) std::cerr << "Error: failed to parse translation unit (code: " << err << ")\n";
        clang_disposeIndex(index);
        return result;
    }

    const unsigned parse_errors = check_diagnostics(tu, verbose);

    // A header that will not parse standalone is skipped outright: no split pieces, no
    // rewritten copy, no manifest entry. Proceeding from a partial AST used to emit both,
    // and the rewritten copy then shadowed the real header for every consumer.
    if (input_is_header && parse_errors > 0) {
        if (verbose)
            out << "Skipping " << input_path << ": not parseable standalone ("
                << parse_errors << " parse error(s))\n";
        write_skipped_header_manifest(unit_dir, preamble_filename, abs_path,
                                      "not parseable standalone");
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        result.success = true;
        return result;
    }

    if (parse_errors > 0 && verbose) {
        // Not a header, so splitting continues; say so plainly rather than leaving the
        // reader to guess whether the output can be trusted.
        out << "Warning: " << parse_errors << " parse error(s) in " << input_path
            << "; splitting anyway, and no stale output will be pruned\n";
    }

    g_unit_source_dir = fs::absolute(abs_path).parent_path().lexically_normal().string();

    // One walk of this translation unit yields the inventory for the file itself and for
    // every header it should split, each seen in the context its includer establishes.
    // Deciding the header set first keeps the walk from recording functions nobody wants.
    const std::vector<std::string> candidates =
        input_is_header ? std::vector<std::string>()
                        : header_split_candidates(tu, abs_path, output_dir, extra_flags,
                                                  verbose, out);

    std::set<std::string> wanted(candidates.begin(), candidates.end());
    wanted.insert(abs_path);

    HarvestMap harvest;
    VarHarvestMap var_harvest;
    VisitorData vd{tu, &wanted, &harvest, &var_harvest};
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, &vd);

    std::vector<FunctionInfo> functions;
    {
        auto hit = harvest.find(abs_path);
        if (hit != harvest.end()) functions = hit->second;
    }

    std::set<std::string> referenced;
    collect_emitted(tu, abs_path, referenced);

    std::vector<VariableInfo> variables;
    {
        auto vit = var_harvest.find(abs_path);
        if (vit != var_harvest.end()) variables = vit->second;
    }

    result = split_unit(tu, abs_path, input_path, source, functions, variables, referenced,
                        input_is_header, output_dir,
                        all_flags, extra_flags, std::string(), parse_errors == 0,
                        verbose, out);

    if (!input_is_header) {
        // Split pieces of this unit's headers include this preamble first, so they compile
        // in the context the header was harvested in.
        const std::string tu_preamble = fs::path(abs_path).stem().string() + "_preamble.h";
        resolve_header_deps(tu, harvest, var_harvest, referenced, candidates, tu_preamble,
                            result, output_dir,
                            all_flags, extra_flags, parse_errors == 0, verbose, out);
    }
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);

    return result;
}

static bool launcher_verbose() {
    const char* val = std::getenv("CPP_SPLITTER_VERBOSE");
    if (val && (std::string(val) == "1" || std::string(val) == "on")) return true;
    return false;
}

// Rewrite the dependency file so it names the files an edit to which should trigger a
// rebuild.
//
// Split pieces include the rewritten copies of headers, never the originals, so that is what
// the compiler records. Those copies live in the build directory and are regenerated only
// when the splitter runs, which happens only once the build system has already decided the
// object is stale -- a loop that never starts. The result is that editing a header rebuilds
// nothing that includes it, and the build reports success while linking stale objects.
//
// The originals are recovered from the `.split` manifests, whose first line is the absolute
// path of the header the copy was generated from. Generated paths are kept as well as the
// originals rather than replaced: a stale copy should also force a rebuild, and a
// prerequisite that no longer exists makes the target dirty, which is the safe direction.
//
// The dependency file also has to be produced on every invocation, not only when something
// recompiled. Only the first piece is given -MD, so an incremental run in which that piece
// is already up to date writes no dependency file at all, and a build system that records
// dependencies itself -- ninja does -- takes the absence as "no dependencies" and discards
// everything it knew. The rewritten file is therefore kept beside the pieces and restored
// when no compilation regenerated it.
// Keep a copy of a depfile the compiler just wrote, so a later run that recompiles nothing
// can put it back.
//
// The build system consumes the depfile and deletes it, so its absence means "already read",
// not "no dependencies" -- but ninja does not keep what it read if the edge runs again and
// produces nothing. An edge that runs and writes no depfile is recorded with *zero*
// dependencies, and the object then survives every later edit to every header it includes.
//
// The split path caches the depfile it rewrites. The fallback path compiled the original
// source with the original -MF, so its depfile is already correct and only needs keeping:
// without this, a unit that fell back once and then split with nothing to recompile lost its
// dependencies entirely. Boost.Geometry's tests do exactly that, and the symptom is a build
// that reports 0.2s where the plain one takes 13s because it has quietly stopped rebuilding.
static void cache_passthrough_depfile(const std::string& dep_path,
                                      const std::string& split_dir,
                                      bool verbose) {
    if (dep_path.empty() || split_dir.empty()) return;
    std::error_code ec;
    if (!fs::exists(dep_path, ec)) return;
    fs::create_directories(split_dir, ec);
    const std::string cache_path = (fs::path(split_dir) / "depfile.cache").string();
    fs::copy_file(dep_path, cache_path, fs::copy_options::overwrite_existing, ec);
    if (verbose && !ec)
        std::cerr << "[cpp-splitter] depfile: cached the fallback's own dependencies\n";
}

static void rewrite_depfile(const std::string& dep_path,
                            const std::string& split_dir,
                            const std::string& input_file,
                            bool verbose) {
    if (dep_path.empty()) return;

    const std::string cache_path = (fs::path(split_dir) / "depfile.cache").string();

    if (!fs::exists(dep_path)) {
        std::error_code copy_ec;
        if (fs::exists(cache_path)) {
            fs::copy_file(cache_path, dep_path,
                          fs::copy_options::overwrite_existing, copy_ec);
            if (verbose && !copy_ec)
                std::cerr << "[cpp-splitter] depfile: restored from cache, nothing"
                             " recompiled\n";
        }
        return;
    }

    // Mirrored header copy -> the original it was generated from.
    std::map<std::string, std::string> original_of;
    std::error_code ec;
    const std::string include_root = split_include_root(split_dir);
    if (fs::exists(include_root)) {
        for (fs::recursive_directory_iterator it(include_root, ec), end; it != end && !ec;
             it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            const std::string path = it->path().string();
            if (path.size() < 6 || path.compare(path.size() - 6, 6, ".split") != 0) continue;
            std::ifstream manifest(path);
            std::string original;
            if (!manifest.is_open() || !std::getline(manifest, original) || original.empty())
                continue;
            original_of[path.substr(0, path.size() - 6)] = original;
        }
    }

    const std::string contents = read_file(dep_path);
    const size_t colon = contents.find(':');
    if (colon == std::string::npos) return;

    std::vector<std::string> deps;
    std::set<std::string> seen;
    auto add = [&](const std::string& d) {
        if (!d.empty() && seen.insert(d).second) deps.push_back(d);
    };

    // Anything the build system should watch: the source itself, then each recorded
    // prerequisite, plus the original behind any generated copy.
    add(fs::absolute(input_file).lexically_normal().string());

    std::istringstream tokens(contents.substr(colon + 1));
    std::string token;
    while (tokens >> token) {
        if (token == "\\") continue;
        add(token);
        auto it = original_of.find(token);
        if (it != original_of.end()) add(it->second);
    }

    std::ostringstream rewritten;
    rewritten << contents.substr(0, colon) << ":";
    for (const auto& d : deps) rewritten << " \\\n  " << d;
    rewritten << "\n";

    {
        std::ofstream ofs(dep_path);
        if (!ofs.is_open()) return;
        ofs << rewritten.str();
    }
    {
        std::ofstream cache(cache_path);
        if (cache.is_open()) cache << rewritten.str();
    }
    if (verbose)
        std::cerr << "[cpp-splitter] depfile: " << deps.size() << " prerequisite(s), "
                  << original_of.size() << " generated header(s) mapped back\n";
}

// Skip the split when nothing it depends on has changed.
//
// The split of a translation unit is a pure function of the source, the flags, and the
// contents of every file it includes. That last list is already written beside the pieces
// as `depfile.cache`, for the build system's benefit. Hashing those contents and storing
// the result next to the output turns re-splitting into a comparison.
//
// This matters because a build system re-runs the launcher whenever a prerequisite's
// timestamp moves, whether or not its contents did. Touching a header, checking out the
// same commit again, or a generator rewriting a file identically all re-ran a full libclang
// parse of every affected translation unit; the pieces then came out byte-for-byte
// identical and nothing was recompiled. The parse was the entire cost.
static std::string split_inputs_hash(const std::string& split_dir,
                                     const std::string& input_file,
                                     const std::vector<std::string>& flags) {
    const std::string dep_cache = (fs::path(split_dir) / "depfile.cache").string();
    if (!fs::exists(dep_cache)) return "";

    std::string material = file_content_hash(input_file);
    if (material.empty()) return "";
    for (const auto& f : flags) material += "\0" + f;

    // Every prerequisite recorded for this unit, in the order the file lists them.
    const std::string deps = read_file(dep_cache);
    const size_t colon = deps.find(':');
    if (colon == std::string::npos) return "";
    std::istringstream tokens(deps.substr(colon + 1));
    std::string token;
    while (tokens >> token) {
        if (token == "\\") continue;
        // A prerequisite that has since vanished must not hash the same as one that is
        // present and empty, so absence gets its own marker.
        const std::string h = file_content_hash(token);
        material += "\0" + token + "\0" + (h.empty() ? "<absent>" : h);
    }

    size_t combined = std::hash<std::string>{}(material);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016zx", combined);
    return std::string(buf);
}

static void write_split_cache(const std::string& split_dir, const std::string& hash,
                              const SplitResult& sr) {
    if (hash.empty()) return;
    std::ofstream ofs((fs::path(split_dir) / "split.cache").string());
    if (!ofs.is_open()) return;
    ofs << hash << "\n" << sr.preamble_filename << "\n";
    ofs << sr.compilable_files.size() << "\n";
    for (const auto& f : sr.compilable_files) ofs << f << "\n";
    ofs << sr.header_obj_dirs.size() << "\n";
    for (const auto& d : sr.header_obj_dirs) ofs << d << "\n";
    ofs << sr.header_obj_files.size() << "\n";
    for (const auto& f : sr.header_obj_files) ofs << f << "\n";
}

static bool read_split_cache(const std::string& split_dir, const std::string& hash,
                             SplitResult& sr) {
    if (hash.empty()) return false;
    std::ifstream ifs((fs::path(split_dir) / "split.cache").string());
    if (!ifs.is_open()) return false;

    std::string stored;
    if (!std::getline(ifs, stored) || stored != hash) return false;
    if (!std::getline(ifs, sr.preamble_filename)) return false;

    auto read_list = [&](std::vector<std::string>& out) {
        std::string count_line;
        if (!std::getline(ifs, count_line)) return false;
        int count = 0;
        try { count = std::stoi(count_line); } catch (...) { return false; }
        for (int i = 0; i < count; ++i) {
            std::string entry;
            if (!std::getline(ifs, entry)) return false;
            // Refuse the cache if anything it names has gone missing.
            if (!fs::exists(entry) && entry.size() > 2 &&
                entry.compare(entry.size() - 2, 2, ".o") != 0)
                return false;
            out.push_back(entry);
        }
        return true;
    };

    if (!read_list(sr.compilable_files)) return false;
    if (!read_list(sr.header_obj_dirs)) return false;
    if (!read_list(sr.header_obj_files)) return false;
    sr.success = true;
    return true;
}

// The linker used to combine a unit's pieces back into one object.
//
// Splitting turns one link of a dozen objects into one of several hundred, so which linker
// does the combining stops being an incidental choice. `CPP_SPLITTER_LINKER` names the
// program; it defaults to `ld` and is passed `-r` either way, since mold, lld and GNU ld all
// spell relocatable output the same. Set it to `mold` or `ld.lld` to try another.
static std::string relocatable_linker() {
    const char* env = std::getenv("CPP_SPLITTER_LINKER");
    if (env && *env) return env;
    return "ld";
}

static int run_as_launcher(int argc, char* argv[]) {
    bool verbose = launcher_verbose();
    std::string compiler = argv[1];
    // Probed lazily, once, the first time flags are built for a parse.
    g_compiler = compiler;

    std::string input_file;
    std::string output_file;
    bool has_c_flag = false;

    bool has_md = false;
    bool has_mmd = false;
    std::string mf_path;
    std::string mt_target;

    std::vector<std::string> other_flags;

    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c") {
            has_c_flag = true;
        } else if (arg == "-o" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "-MD") {
            has_md = true;
        } else if (arg == "-MMD") {
            has_mmd = true;
        } else if (arg == "-MF" && i + 1 < argc) {
            mf_path = argv[++i];
        } else if (arg == "-MT" && i + 1 < argc) {
            mt_target = argv[++i];
        } else if (arg == "-MQ" && i + 1 < argc) {
            ++i;
        } else if (is_source_file(arg)) {
            input_file = arg;
        } else {
            other_flags.push_back(arg);
        }
    }

    if (!has_c_flag || input_file.empty()) {
        std::string cmd;
        for (int i = 1; i < argc; i++) {
            if (i > 1) cmd += " ";
            cmd += shell_quote(argv[i]);
        }
        if (verbose) std::cerr << "[cpp-splitter] passthrough: " << cmd << "\n";
        return run_command_quiet(cmd);
    }

    if (!fs::exists(input_file)) {
        std::cerr << "cpp-splitter: source file not found: " << input_file << "\n";
        return 1;
    }

    if (verbose) {
        std::cerr << "[cpp-splitter] splitting: " << input_file << "\n";
        std::cerr << "[cpp-splitter] flags (" << other_flags.size() << "):";
        for (const auto& f : other_flags) std::cerr << " " << f;
        std::cerr << "\n";
        const std::string& probed = cached_driver_standard();
        if (!probed.empty())
            std::cerr << "[cpp-splitter] parse standard: " << probed << " (probed)\n";
    }

    std::string split_dir;
    if (!output_file.empty()) {
        split_dir = fs::absolute(output_file).string() + ".split";
    } else {
        split_dir = fs::absolute(fs::path(input_file).stem().string() + ".split").string();
    }

    auto split_flags = other_flags;
    if (compiler == "tipi-compiler-driver") {
      auto actual_compiler = *split_flags.begin();
      split_flags = std::vector<std::string>(split_flags.begin()+1, split_flags.end());
    }
    
    const std::string inputs_hash = split_inputs_hash(split_dir, input_file, split_flags);

    SplitResult sr;
    bool reused = read_split_cache(split_dir, inputs_hash, sr);
    if (reused) {
        if (verbose)
            std::cerr << "[cpp-splitter] inputs unchanged, reusing the existing split\n";
        for (const auto& hdr : sr.header_obj_dirs) (void)hdr;
    } else {
        sr = do_split(input_file, split_dir, split_flags, verbose);
    }


    auto build_passthrough_cmd = [&]() {
        std::string cmd;
        for (int i = 1; i < argc; i++) {
            if (i > 1) cmd += " ";
            cmd += shell_quote(argv[i]);
        }
        return cmd;
    };

    if (!sr.success) {
        // Not gated on verbose. A fallback means the tool did nothing it exists to do, and
        // the build succeeds either way -- so a silent one is a silent regression. The
        // failure that motivated this said so: a stale prefix PCH made every header edit
        // fall back, and the only trace was in a log nobody was reading.
        std::cerr << "[cpp-splitter] splitting failed, falling back to normal compilation\n";
        std::string cmd = build_passthrough_cmd();
        if (verbose) std::cerr << "[cpp-splitter] passthrough: " << cmd << "\n";
        const int rc = run_command_quiet(cmd);
        if (rc == 0) cache_passthrough_depfile(mf_path, split_dir, verbose);
        return rc;
    }

    // A translation unit can have nothing of its own to split and still be worth splitting:
    // when all of its code arrives through an included file, the pieces are that file's, and
    // they are held in header_obj_files rather than compilable_files.
    if (sr.compilable_files.empty() && sr.header_obj_files.empty()) {
        std::string cmd = build_passthrough_cmd();
        if (verbose) std::cerr << "[cpp-splitter] nothing to split, passthrough: " << cmd << "\n";
        const int rc = run_command_quiet(cmd);
        if (rc == 0) cache_passthrough_depfile(mf_path, split_dir, verbose);
        return rc;
    }

    if (compiler == "tipi-compiler-driver") {
      std::cout << "BEGIN compiler_with_driver is: " << std::endl;
      auto pch_flags = other_flags;
      auto actual_compiler = *pch_flags.begin();
      pch_flags = std::vector<std::string>(pch_flags.begin()+1, pch_flags.end());
      std::cout << "compiler_with_driver is: " << compiler << " " << actual_compiler<< std::endl;
      pch_flags.insert(pch_flags.begin(),
                       {"-I" + split_include_root(split_dir),
                        "-I" + fs::absolute(input_file).parent_path().string()});
      build_pch(sr.preamble_filename, actual_compiler, "", pch_flags, split_dir, verbose, std::cerr);
    } else {
      auto pch_flags = other_flags;
      pch_flags.insert(pch_flags.begin(),
                       {"-I" + split_include_root(split_dir),
                        "-I" + fs::absolute(input_file).parent_path().string()});
      build_pch(sr.preamble_filename, compiler, "", pch_flags, split_dir, verbose, std::cerr);
    }

    // Whether the dependency-tracking flags have been handed to some compile yet.
    bool dep_flags_placed = false;
    std::vector<std::string> obj_files;
    std::vector<CompileJob> parallel_jobs;
    int launcher_skipped = 0;
    bool split_build_failed = false;

    for (size_t fi = 0; fi < sr.compilable_files.size(); ++fi) {
        const auto& cpp = sr.compilable_files[fi];
        std::string obj = cpp.substr(0, cpp.size() - 4) + ".o";
        obj_files.push_back(obj);

        if (!needs_recompile(cpp, obj, sr.preamble_filename)) {
            if (verbose) std::cerr << "[cpp-splitter] up-to-date: " << cpp << "\n";
            ++launcher_skipped;
            continue;
        }

        std::string cmd = shell_quote(compiler);

        // The split tree has to precede the project's own include directories. Its
        // rewritten headers are the ones whose definitions were moved into split files;
        // if an original wins the lookup instead, its definitions come back and collide
        // with the pieces that now also define them.
        cmd += " -I" + shell_quote(split_dir);
        cmd += " -I" + shell_quote(split_include_root(split_dir));
        for (const auto& hdr_dir : sr.header_obj_dirs)
            cmd += " -I" + shell_quote(hdr_dir);

        // The preamble is the source's own text, so it carries the source's quote includes.
        // It no longer sits in the source's directory, where those would have resolved, so
        // that directory has to be on the include path -- after the split tree, so the
        // rewritten headers still win.
        cmd += " -I" + shell_quote(fs::absolute(input_file).parent_path().string());

        for (const auto& f : other_flags)
            cmd += " " + shell_quote(f);

        if (fi == 0 && (has_md || has_mmd)) {
            dep_flags_placed = true;
            cmd += has_mmd ? " -MMD" : " -MD";
            if (!mf_path.empty())
                cmd += " -MF " + shell_quote(mf_path);
            std::string mt = mt_target.empty() ? output_file : mt_target;
            if (!mt.empty())
                cmd += " -MT " + shell_quote(mt);
        }

        cmd += " -c -o " + shell_quote(obj) + " " + shell_quote(cpp);

        if (fi == 0 && (has_md || has_mmd)) {
            if (verbose) std::cerr << "[cpp-splitter] compile (seq): " << cmd << "\n";
            int ret = run_command_quiet(cmd);
            if (ret != 0) {
                std::cerr << "cpp-splitter: compilation failed for split file: " << cpp << "\n";
                split_build_failed = true;
                break;
            }
        } else {
            parallel_jobs.push_back({cmd, cpp, obj});
        }
    }

    if (!split_build_failed && !parallel_jobs.empty()) {
        if (verbose) std::cerr << "[cpp-splitter] compiling " << parallel_jobs.size() << " split file(s) in parallel\n";
        auto results = compile_parallel(parallel_jobs, verbose, std::cerr);
        for (const auto& r : results) {
            if (r.exit_code != 0) {
                std::cerr << "cpp-splitter: compilation failed for split file: " << r.source_file << "\n";
                if (!r.stderr_output.empty()) std::cerr << r.stderr_output;
                split_build_failed = true;
                break;
            }
        }
    }

    if (output_file.empty())
        output_file = fs::path(input_file).stem().string() + ".o";

    if (!split_build_failed) {
        std::vector<CompileJob> hdr_compile_jobs;
        for (const auto& hobj : sr.header_obj_files) {
            std::string hcpp = hobj.substr(0, hobj.size() - 2) + ".cpp";
            if (!fs::exists(hcpp)) continue;
            if (!fs::exists(hobj) || needs_recompile(hcpp, hobj, "")) {
                std::string cmd = shell_quote(compiler);
                // The unit's own preamble lives at the root of the split directory, and the
                // header pieces include it for context. The split tree precedes the
                // project's own include directories for the reason above.
                cmd += " -I" + shell_quote(split_dir);
                cmd += " -I" + shell_quote(split_include_root(split_dir));
                for (const auto& hdr_dir : sr.header_obj_dirs)
                    cmd += " -I" + shell_quote(hdr_dir);
                cmd += " -I" + shell_quote(fs::absolute(input_file).parent_path().string());
                for (const auto& f : other_flags)
                    cmd += " " + shell_quote(f);

                // The dependency flags normally go to the unit's first piece. A unit whose
                // whole body arrives through an included file has no pieces of its own, so
                // they would go nowhere and the build system would record no dependencies
                // at all for it -- leaving an edit to that include undetected. Attach them
                // to the first header piece instead.
                if (!dep_flags_placed && (has_md || has_mmd)) {
                    cmd += has_mmd ? " -MMD" : " -MD";
                    if (!mf_path.empty()) cmd += " -MF " + shell_quote(mf_path);
                    const std::string mt = mt_target.empty() ? output_file : mt_target;
                    if (!mt.empty()) cmd += " -MT " + shell_quote(mt);
                    dep_flags_placed = true;
                }

                cmd += " -c -o " + shell_quote(hobj) + " " + shell_quote(hcpp);
                hdr_compile_jobs.push_back({cmd, hcpp, hobj});
            }
            obj_files.push_back(hobj);
            if (verbose) std::cerr << "[cpp-splitter] header dep .o: " << hobj << "\n";
        }
        if (!hdr_compile_jobs.empty()) {
            if (verbose) std::cerr << "[cpp-splitter] compiling " << hdr_compile_jobs.size() << " header dep file(s)\n";
            auto hdr_results = compile_parallel(hdr_compile_jobs, verbose, std::cerr);
            for (const auto& r : hdr_results) {
                if (r.exit_code != 0) {
                    std::cerr << "cpp-splitter: compilation failed for header dep: " << r.source_file << "\n";
                    if (!r.stderr_output.empty()) std::cerr << r.stderr_output;
                    split_build_failed = true;
                    break;
                }
            }
        }
    }

    if (!split_build_failed) {
        rewrite_depfile(mf_path, split_dir, input_file, verbose);
        if (!reused)
            write_split_cache(split_dir,
                              split_inputs_hash(split_dir, input_file, split_flags), sr);
    }

    if (!split_build_failed) {
        bool need_link = (launcher_skipped < (int)sr.compilable_files.size()) || !sr.header_obj_files.empty() || !fs::exists(output_file);

        if (!need_link) {
            // Nothing changed the object's contents, but the build system decides staleness
            // from timestamps, so leaving it older than an input it no longer differs from
            // means it is judged dirty on every invocation for ever. Touching it records
            // that this build considered the object and found it current.
            //
            // A build system that compared content rather than modification time would not
            // need this, and would also stop rebuilding everything downstream when a split
            // piece is regenerated byte-for-byte identically -- which is the common case
            // here, since the splitter rewrites a piece only when its text actually differs.
            std::error_code touch_ec;
            fs::last_write_time(output_file, fs::file_time_type::clock::now(), touch_ec);
            if (verbose)
                std::cerr << "[cpp-splitter] all up-to-date, skipping link: " << output_file
                          << (touch_ec ? " (could not update timestamp)" : " (timestamp updated)")
                          << "\n";
        } else if (obj_files.size() == 1) {
            if (verbose) std::cerr << "[cpp-splitter] single .o, copying " << obj_files[0] << " -> " << output_file << "\n";
            fs::copy_file(obj_files[0], output_file, fs::copy_options::overwrite_existing);
        } else {
            const std::string linker = relocatable_linker();
            std::string cmd = shell_quote(linker) + " -r -o " + shell_quote(output_file);
            for (const auto& obj : obj_files)
                cmd += " " + shell_quote(obj);
            if (verbose) std::cerr << "[cpp-splitter] " << linker << " -r: " << cmd << "\n";
            int ret = run_command_quiet(cmd);
            if (ret != 0) {
                std::cerr << "cpp-splitter: relocatable link failed using '" << linker
                          << "'\n";
                split_build_failed = true;
            }
        }
    }

    if (split_build_failed) {
        std::cerr << "[cpp-splitter] split build failed, falling back to normal compilation\n";
        std::string cmd = build_passthrough_cmd();
        if (verbose) std::cerr << "[cpp-splitter] passthrough: " << cmd << "\n";
        const int rc = run_command_quiet(cmd);
        if (rc == 0) cache_passthrough_depfile(mf_path, split_dir, verbose);
        return rc;
    }

    if (verbose) std::cerr << "[cpp-splitter] done: " << output_file << "\n";
    return 0;
}

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <input.cpp> [output_dir] [options] [-- <clang_flags>...]\n"
              << "       " << prog << " <compiler> [compiler_flags...] -c -o <output.o> <source.cpp>\n"
              << "\n"
              << "Mode 1 - Split (and optionally compile):\n"
              << "  Parses the input .cpp file using libclang AST and generates\n"
              << "  one file per function/method implementation.\n"
              << "\n"
              << "  Arguments:\n"
              << "    input.cpp      The C++ source file to split\n"
              << "    output_dir     Directory for output files (default: ./output)\n"
              << "\n"
              << "  Options:\n"
              << "    --compile      Compile and link the split files into a binary\n"
              << "    -o <binary>    Output binary name (default: <stem>.out)\n"
              << "    --cxx <comp>   C++ compiler to use (default: g++)\n"
              << "    -- <flags>     Extra flags passed to clang parser\n"
              << "\n"
              << "Mode 2 - Compiler Launcher (CMAKE_CXX_COMPILER_LAUNCHER):\n"
              << "  When the first argument is not a source file, acts as a\n"
              << "  compiler wrapper. Splits the source, compiles\n"
              << "  each piece, and combines them into a single .o via relocatable\n"
              << "  linking.\n"
              << "\n"
              << "  Non-compilation commands are passed through transparently.\n"
              << "\n"
              << "  CMake usage:\n"
              << "    cmake -DCMAKE_CXX_COMPILER_LAUNCHER=/path/to/" << prog << " ..\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << " src/app.cpp output                          # split only\n"
              << "  " << prog << " src/app.cpp output --compile -o myapp       # split + compile + link\n"
              << "  " << prog << " g++ -std=c++17 -c -o foo.o foo.cpp          # launcher mode\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string first_arg = argv[1];

    if (!first_arg.empty() && first_arg[0] != '-' && !is_splittable_file(first_arg)) {
        return run_as_launcher(argc, argv);
    }

    // Everything this driver compiles it compiles with -std=c++17, so the parse has to use
    // the same one rather than whatever the compiler defaults to.
    g_forced_standard = "-std=c++17";

    std::string input_path = argv[1];
    std::string output_dir = "output";
    std::vector<std::string> extra_flags;
    bool do_compile = false;
    std::string output_binary;
    std::string cxx_compiler = "g++";

    int i = 2;
    if (i < argc && argv[i][0] != '-') {
        output_dir = argv[i];
        ++i;
    }

    while (i < argc) {
        std::string arg = argv[i];
        if (arg == "--") {
            ++i;
            for (; i < argc; ++i)
                extra_flags.emplace_back(argv[i]);
            break;
        } else if (arg == "--compile") {
            do_compile = true;
            ++i;
        } else if (arg == "-o" && i + 1 < argc) {
            output_binary = argv[i + 1];
            i += 2;
        } else if (arg == "--cxx" && i + 1 < argc) {
            cxx_compiler = argv[i + 1];
            // The parse has to see this compiler's system headers, not some other one's:
            // GCC's and clang's are not interchangeable, and the pieces are compiled with
            // this one.
            g_compiler = cxx_compiler;
            i += 2;

        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!fs::exists(input_path)) {
        std::cerr << "Error: input file does not exist: " << input_path << "\n";
        return 1;
    }

    std::string stem = fs::path(input_path).stem().string();
    if (output_binary.empty())
        output_binary = stem + ".out";

    load_header_manifests(output_dir);

    SplitResult sr = do_split(input_path, output_dir, extra_flags, true);

    if (!sr.success) {
        std::cerr << "Warning: splitting failed, falling back to normal compilation\n";
        if (do_compile) {
            std::string cmd = cxx_compiler + " -std=c++17";
            for (const auto& f : extra_flags) cmd += " " + f;
            cmd += " -o " + output_binary + " " + input_path;
            std::cout << "\n--- Compiling without splitting ---\n\n";
            std::cout << "  $ " << cmd << "\n";
            int ret = run_command(cmd);
            if (ret != 0) {
                std::cerr << "Error: compilation failed\n";
                return 1;
            }
            std::cout << "\nBuild successful: " << output_binary << "\n";
        }
        return 0;
    }

    if (sr.compilable_files.empty()) return 0;

    if (do_compile) {
        std::cout << "\n--- Compiling split files ---\n\n";

        // The preamble is the source's own text, so it carries the source's includes: it
        // needs the mirrored header tree and the source's directory just as the split
        // pieces do, or a quote include of a sibling header cannot be found.
        std::vector<std::string> pch_flags = {"-std=c++17"};
        pch_flags.push_back("-I" + split_include_root(output_dir));
        pch_flags.push_back("-I" + fs::absolute(input_path).parent_path().string());
        for (const auto& f : extra_flags) pch_flags.push_back(f);
        bool pch_ok = build_pch(sr.preamble_filename, cxx_compiler, "", pch_flags, output_dir, true, std::cout);
        if (pch_ok) std::cout << "\n";

        std::vector<CompileJob> jobs;
        std::vector<std::string> obj_files;
        int skipped = 0;

        for (const auto& cpp_file : sr.compilable_files) {
            std::string obj_file = cpp_file.substr(0, cpp_file.size() - 4) + ".o";
            obj_files.push_back(obj_file);

            if (!needs_recompile(cpp_file, obj_file, sr.preamble_filename)) {
                std::cout << "  (up-to-date) " << cpp_file << "\n";
                ++skipped;
                continue;
            }

            std::string cmd = cxx_compiler + " -std=c++17 -c"
                              " -I" + output_dir +
                              " -I" + split_include_root(output_dir);
            for (const auto& hdr_dir : sr.header_obj_dirs)
                cmd += " -I" + hdr_dir;
            cmd += " -I" + fs::absolute(input_path).parent_path().string();
            cmd += " -o " + obj_file +
                   " " + cpp_file;

            for (const auto& f : extra_flags)
                cmd += " " + f;

            jobs.push_back({cmd, cpp_file, obj_file});
        }

        bool compile_ok = true;
        if (jobs.empty()) {
            std::cout << "\n  All " << skipped << " file(s) up-to-date, nothing to recompile.\n";
        } else {
            if (skipped > 0)
                std::cout << "  (" << skipped << " file(s) up-to-date, recompiling " << jobs.size() << ")\n";
            auto results = compile_parallel(jobs, true);
            for (const auto& r : results) {
                if (r.exit_code != 0) {
                    std::cerr << "Error: compilation failed for " << r.source_file << "\n";
                    if (!r.stderr_output.empty()) std::cerr << r.stderr_output;
                    compile_ok = false;
                }
            }
        }

        if (compile_ok && !sr.header_obj_files.empty()) {
            std::cout << "\n--- Compiling header dependencies ---\n\n";

            std::vector<CompileJob> hdr_jobs;
            for (size_t hi = 0; hi < sr.header_obj_files.size(); ++hi) {
                const auto& hobj = sr.header_obj_files[hi];
                std::string hcpp = hobj.substr(0, hobj.size() - 2) + ".cpp";
                if (!fs::exists(hcpp)) continue;

                std::string hdr_preamble;
                for (const auto& hdir : sr.header_obj_dirs) {
                    for (const auto& entry : fs::directory_iterator(hdir)) {
                        std::string fn = entry.path().filename().string();
                        if (is_header_file(fn) && fn.find("_preamble") == std::string::npos) {
                            hdr_preamble = entry.path().string();
                            break;
                        }
                    }
                    if (!hdr_preamble.empty()) break;
                }

                if (!needs_recompile(hcpp, hobj, hdr_preamble)) {
                    std::cout << "  (up-to-date) " << hcpp << "\n";
                    continue;
                }

                std::string cmd = cxx_compiler + " -std=c++17 -c -I" + output_dir +
                                  " -I" + split_include_root(output_dir);
                for (const auto& hdr_dir : sr.header_obj_dirs)
                    cmd += " -I" + hdr_dir;
                cmd += " -I" + fs::absolute(input_path).parent_path().string();
                cmd += " -o " + hobj + " " + hcpp;
                for (const auto& f : extra_flags)
                    cmd += " " + f;

                hdr_jobs.push_back({cmd, hcpp, hobj});
            }

            if (!hdr_jobs.empty()) {
                auto hdr_results = compile_parallel(hdr_jobs, true);
                for (const auto& r : hdr_results) {
                    if (r.exit_code != 0) {
                        std::cerr << "Error: compilation failed for header dep " << r.source_file << "\n";
                        if (!r.stderr_output.empty()) std::cerr << r.stderr_output;
                        compile_ok = false;
                    }
                }
            }
        }

        if (compile_ok) {
            std::cout << "\n--- Linking ---\n\n";

            for (const auto& hobj : sr.header_obj_files) {
                if (fs::exists(hobj)) {
                    obj_files.push_back(hobj);
                    std::cout << "  (header dep) " << hobj << "\n";
                }
            }

            std::string link_cmd = cxx_compiler + " -o " + output_binary;
            for (const auto& obj : obj_files)
                link_cmd += " " + obj;

            int ret = run_command(link_cmd);
            if (ret != 0) {
                std::cerr << "Error: linking failed\n";
                return 1;
            }

            std::cout << "\nBuild successful: " << output_binary << "\n";
        } else {
            std::cerr << "Warning: split compilation failed, falling back to normal compilation\n";
            std::string cmd = cxx_compiler + " -std=c++17";
            for (const auto& f : extra_flags) cmd += " " + f;
            cmd += " -o " + output_binary + " " + input_path;
            std::cout << "\n--- Compiling without splitting ---\n\n";
            std::cout << "  $ " << cmd << "\n";
            int ret = run_command(cmd);
            if (ret != 0) {
                std::cerr << "Error: compilation failed\n";
                return 1;
            }
            std::cout << "\nBuild successful (fallback): " << output_binary << "\n";
        }
    }

    return 0;
}
