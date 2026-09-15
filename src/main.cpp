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

// The flags of a compile that change where the driver looks for its system headers, and so
// have to be on the probe too: p4c is compiled with -stdlib=libc++, and a probe without it
// handed libclang libstdc++ 13's <chrono>, which clang 13 cannot parse in C++20 -- every
// unit parsed with errors and the harvest was wrong. TODO/47.
static std::vector<std::string> include_path_flags(const std::vector<std::string>& flags) {
    std::vector<std::string> out;
    for (size_t i = 0; i < flags.size(); ++i) {
        const std::string& f = flags[i];
        // The two-token forms first, or `-isysroot <sdk>` is pushed twice -- once bare by
        // the prefix match below, once with its value -- and the probe runs with a sysroot
        // called "-isysroot": on macOS with Homebrew's clang that lost the SDK's usr/include
        // and every unit parsed with errors in <ctype.h>. TODO/43.
        if ((f == "-target" || f == "-isysroot" || f == "--sysroot" || f == "-stdlib") &&
            i + 1 < flags.size()) {
            out.push_back(f);
            out.push_back(flags[++i]);
            continue;
        }
        if (f.rfind("-stdlib=", 0) == 0 || f.rfind("--gcc-toolchain", 0) == 0 ||
            f.rfind("--sysroot", 0) == 0 || f.rfind("--target=", 0) == 0 ||
            f == "-m32" || f == "-m64" || f == "-nostdinc" || f == "-nostdinc++" ||
            f == "-nostdlibinc" || f.rfind("-isysroot", 0) == 0)
            out.push_back(f);
    }
    return out;
}

static std::string shell_quote(const std::string& s);

static std::vector<std::string> detect_system_includes(const std::string& compiler = "g++",
                                                       const std::vector<std::string>& flags = {}) {
    std::vector<std::string> includes;
    std::string cmd = compiler;
    for (const auto& f : flags) cmd += " " + shell_quote(f);
    cmd += " -E -x c++ /dev/null -v 2>&1";
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
            // Normalised: Homebrew's clang reports `<prefix>/bin/../include/c++/v1`, and
            // libclang names files under `<prefix>/include/c++/v1`, so a prefix match on the
            // raw line called every libc++ header a project header. TODO/43.
            if (!path.empty() && fs::exists(path))
                includes.push_back(fs::path(path).lexically_normal().string());
        }
    }
    if (std::getenv("CPP_SPLITTER_VERBOSE")) {
        std::cerr << "[cpp-splitter] system includes (" << cmd << "):";
        for (const auto& inc : includes) std::cerr << " " << inc;
        std::cerr << "\n";
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

// A C++20 named module unit, read off the source text. TODO/43.
//
// Only the primary interface unit is split as a module: its non-inline bodies become
// implementation units of the module, compiled against the BMI the rewritten interface
// produces. An implementation unit (`module M;` with no `export`) is compiled whole for now;
// an importer is an ordinary translation unit whose pieces carry its `import` lines.
struct ModuleUnit {
    bool interface = false;   // `export module M;`
    bool implementation = false;   // `module M;`
    std::string name;
    // The global module fragment: what stands between `module;` and the module declaration.
    // Preprocessor directives only, by the standard, so it is replayed verbatim by every
    // piece ahead of its own module declaration.
    std::string gmf;
};

// The unit being split, when it is a module interface unit, and the imports of the unit
// being split whatever it is. Set per launcher invocation.
static ModuleUnit g_module_unit;
static std::vector<std::string> g_unit_imports;

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
    unsigned sloc_raw = 0;            // raw source location: which inclusion of `file`, TODO/44 (B)
    std::vector<ScopeEntry> scope_chain;
    bool is_template;
    bool is_static;
    // Defined inside `extern "C"` -- by block, or by a macro such as OpenCV's CV_IMPL. The
    // piece has to say so again, or it defines a C++-linkage function of the same name and
    // the C-linkage declaration left in the preamble resolves to nothing.
    bool c_linkage = false;
    // A static whose unqualified name is also that of another function declared anywhere in
    // the translation unit. The rename that lets pieces share a static is textual and would
    // reach both, so such a static stays in the preamble instead.
    bool name_shared = false;
    // References a static variable whose type no other translation unit can name. The
    // variable moves whole to the definitions header, and so does this function, whatever
    // its linkage: a `static` one is renamed like any other so the pieces can still call it.
    bool kept_with_variable = false;
    // `= default;` or `= delete;` out of line: the extent was carried through the `;`, and
    // there is no body to re-slice.
    bool is_defaulted_or_deleted = false;
    // The function's type carries an exception specification -- `throw()`, `noexcept` --
    // that the definition's own text may not spell: a definition over a declaration in a
    // system header inherits it, as p4c's `void free(void *)` does from <stdlib.h>. The
    // compiler allows the omission only against a declaration from a system header, so no
    // declaration is written into the preamble for such a function: the one it inherited
    // from is already there. TODO/47.
    bool has_exception_spec = false;
    bool has_prior_declaration = false;   // an earlier declaration in the translation unit
    // The body holds a function-local `static` that is not a constant: state. Kept in the
    // preamble with internal linkage, the function is one per piece and so is its state.
    // p4c's lib/cstring.cpp interns strings through `auto &cache()` in an unnamed namespace,
    // whose local `static node_hash_set g_cache` became one cache per piece, and cstrings
    // compared by pointer stopped comparing equal. Such a unit is declined. TODO/47.
    bool has_local_static = false;
    // A header definition this unit does not emit, of a shape it would otherwise split:
    // the unit's copy of the header declares it and no piece is written. The copy is then
    // unchanged by an edit to the body, and so is the PCH built from it and every piece
    // compiled against that PCH. p4c's cstring::size() is defined in class and emitted by
    // 14 of 218 units; kept in the other 204 copies, one line in its body rebuilt 194 PCHs
    // and 9689 pieces (TODO/48).
    bool declare_only = false;
    // Named in the unit, so not declared only, and referring to a function this
    // translation unit declares but does not define -- a template with no definition in
    // reach, a forwarder to another object -- so a piece forced into existence would carry
    // a reference the plain build never made. Kept with its body (TODO/49).
    bool lacks_definition = false;

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
    unsigned sloc_raw = 0;            // raw source location: which inclusion of `file`, TODO/44 (B)
    std::string type_spelling;        // for the declaration left behind, when the text cannot give one
    unsigned name_offset = 0;         // where the declarator's name is written
    std::vector<ScopeEntry> scope_chain;
    bool is_member = false;           // out-of-class static data member: the class declares it
    bool move_out = false;            // may exist in only one object
    // The names of the other declarators of the same declaration, when several are moved
    // together (`static bool done_init, started_init;`): renamed like the first. TODO/47.
    std::vector<std::string> other_names;
    // Of an empty type -- a captureless lambda, an empty struct: one copy per piece is the
    // same object for every purpose, so a `static` one may stay in the preamble.
    bool is_empty_type = false;
    // Kept where it is and marked `inline`, rather than moved to the definitions header.
    // Only from C++17, where inline variables exist.
    bool inline_in_place = false;
    bool internal_linkage = false;    // `static`, or enclosed by an unnamed namespace
    bool type_lacks_linkage = false;  // its type cannot be named from another translation unit
    std::string usr;                  // what a reference to it records (see EmitGraph)
    // A `static` whose type no other translation unit can name. It cannot be shared by the
    // pieces and must not be copied into each, so it moves whole -- `static` kept, type
    // definition included, no declaration left behind -- to the definitions header, and every
    // function that references it goes there with it. TODO/44.
    bool anchors_users = false;
    // const or constexpr, asked of the type rather than read off the text. The keyword is
    // routinely a macro -- Boost.Filesystem writes BOOST_CONSTEXPR_OR_CONST -- and such a
    // variable has to stay where its users can see it as a constant expression.
    bool is_const = false;
    bool in_unnamed_ns = false;       // an unnamed namespace encloses it beyond the innermost run
    // Innermost consecutive unnamed namespaces around it. A variable in one is moved out
    // and renamed like a static, its definition hoisted into the enclosing named scope and
    // the `extern` left behind placed there too, by closing and reopening the unnamed
    // namespaces around it. p4c's bison-generated ir-generator.cpp keeps its parser state
    // -- `static IrNamespace *current_namespace` -- in one; left in the preamble it was one
    // per piece and the generated IR header had every method twice. TODO/47.
    unsigned unnamed_ns_depth = 0;
    // Moved out and renamed, the way a `static` function in a .cpp already is. Only for the
    // translation unit's own source: see TODO 26.
    bool rename_and_move = false;
    std::string replacement;          // what the preamble gets in place of the definition
};

// Set by do_split() when it declined the unit -- decided before anything was written that it
// cannot be split correctly -- as opposed to failed. The two are reported apart: a fallback
// is a defect, a decline is a limit the tool stated.
static bool g_declined = false;

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

// The last graph's references, kept for prepare_functions(): which definitions refer to a
// given USR. A variable of a type no other translation unit can name anchors every function
// that references it to the definitions unit (TODO/44), and this is how they are found.
static std::map<std::string, std::set<std::string>> g_references;

// Definitions that refer to a function this translation unit declares and does not define,
// outside the system headers: a template whose definition is not in reach -- Boost.Math's
// real_concept.hpp calls `boost::math::acosh` with only math_fwd.hpp read -- or a forwarder
// to a function another object defines. The compiler emits such a body only if something
// uses it; a piece forced into existence emits it whatever happens and the reference goes
// unresolved. And the functions defined here with vague linkage, which a body that calls
// them emits along with itself: what they lack, their callers lack too (TODO/49).
static std::set<std::string> g_lacks_definition;
static std::set<std::string> g_vague_defined;

static CXChildVisitResult record_reference(CXCursor node, CXCursor, CXClientData payload) {
    auto* g = static_cast<EmitGraph*>(payload);
    const CXCursorKind k = clang_getCursorKind(node);
    if (k == CXCursor_CallExpr || k == CXCursor_DeclRefExpr || k == CXCursor_MemberRefExpr) {
        CXCursor ref = clang_getCursorReferenced(node);
        if (!clang_Cursor_isNull(ref)) {
            std::string target = cx_to_string(clang_getCursorUSR(ref));
            if (!target.empty() && !g->stack.empty())
                g->calls[g->stack.back()].insert(target);
            const CXCursorKind rk = clang_getCursorKind(ref);
            const bool fn_kind =
                rk == CXCursor_FunctionDecl || rk == CXCursor_CXXMethod ||
                rk == CXCursor_Constructor || rk == CXCursor_Destructor ||
                rk == CXCursor_ConversionFunction || rk == CXCursor_FunctionTemplate;
            if (fn_kind && !target.empty() && !g->stack.empty()) {
                const CXCursor tmpl = clang_getSpecializedCursorTemplate(ref);
                CXCursor def = clang_getCursorDefinition(ref);
                if (clang_Cursor_isNull(def) && !clang_Cursor_isNull(tmpl))
                    def = clang_getCursorDefinition(tmpl);
                if (clang_Cursor_isNull(def)) {
                    const CXCursor where = clang_Cursor_isNull(tmpl) ? ref : tmpl;
                    if (!clang_Location_isInSystemHeader(clang_getCursorLocation(where)))
                        g_lacks_definition.insert(g->stack.back());
                } else if (clang_Cursor_isFunctionInlined(def) || !clang_Cursor_isNull(tmpl) ||
                           clang_getCursorLinkage(def) != CXLinkage_External) {
                    g_vague_defined.insert(target);
                }
            }
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
    g_lacks_definition.clear();
    g_vague_defined.clear();
    clang_visitChildren(clang_getTranslationUnitCursor(tu), build_emit_graph, &graph);
    g_references = graph.calls;
    // A body that calls a vague-linkage function lacking a definition emits that function
    // with itself, and lacks the same definition.
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& entry : graph.calls) {
            if (g_lacks_definition.count(entry.first)) continue;
            for (const auto& t : entry.second)
                if (g_lacks_definition.count(t) && g_vague_defined.count(t)) {
                    g_lacks_definition.insert(entry.first);
                    changed = true;
                    break;
                }
        }
    }

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

// --- TODO/44 (B): a header included more than once with no include guard ------------------
//
// Such a file is one file and several inclusions, each under its own macro state, and one
// rewritten copy cannot serve them all: OpenCV's arithm.simd.hpp defines the SIMD kernels the
// first time it is read and the cv::hal dispatchers the second. Each inclusion is therefore
// split on its own, the second and later ones into copies under include/_inclusion/<n>/, and
// the unit's preamble is rewritten to name those copies on the later #include lines.
//
// A definition is attributed to an inclusion through its raw source location. clang's source
// manager gives every inclusion of a file an entry of its own, whose base offset the raw
// location encodes, and creates the entries in preprocessing order. The base is what remains
// of a raw location once the offset within the file is taken away. A location produced by a
// macro expansion has an entry of its own, created while the inclusion was being read, so it
// falls between the base of the inclusion it belongs to and the base of the next.
//
// Entries loaded from a precompiled header sit at the top of the offset range and the ones
// the parse creates at the bottom, so a loaded base is ordered before every local one. The
// prefix PCH holds the unit's whole include block, so both inclusions of a pair are either
// loaded from it, in order, or -- a pair written after the first line of code -- both created
// by the parse, in order.
static const unsigned k_macro_id_bit = 1u << 31;
static const unsigned k_loaded_offset_floor = 1u << 30;

static unsigned raw_sloc_offset(CXSourceLocation loc) { return loc.int_data & ~k_macro_id_bit; }
static bool sloc_is_macro(CXSourceLocation loc) { return (loc.int_data & k_macro_id_bit) != 0; }

// file -> the base offset of every source-manager entry seen for it: one per inclusion read.
static std::map<std::string, std::set<unsigned>> g_file_bases;

static void note_inclusion_base(const std::string& file, CXSourceLocation loc) {
    if (sloc_is_macro(loc)) return;
    unsigned off = 0;
    clang_getFileLocation(loc, nullptr, nullptr, nullptr, &off);
    g_file_bases[file].insert(raw_sloc_offset(loc) - off);
}

// The bases of a file in preprocessing order: loaded before local, ascending within each.
static std::vector<unsigned> ordered_inclusion_bases(const std::string& file) {
    std::vector<unsigned> loaded, local;
    auto it = g_file_bases.find(file);
    if (it == g_file_bases.end()) return {};
    for (unsigned b : it->second) (b >= k_loaded_offset_floor ? loaded : local).push_back(b);
    loaded.insert(loaded.end(), local.begin(), local.end());
    return loaded;
}

// The 1-based inclusion a raw location belongs to: the last base of its kind not after it.
static unsigned inclusion_of_raw(const std::vector<unsigned>& bases, unsigned raw) {
    const bool loaded = raw >= k_loaded_offset_floor;
    unsigned found = 0;
    for (size_t i = 0; i < bases.size(); ++i) {
        if ((bases[i] >= k_loaded_offset_floor) != loaded) continue;
        if (bases[i] <= raw) found = static_cast<unsigned>(i + 1);
    }
    return found;
}

// The harvest and g_split_headers key of inclusion n of a file: the path itself for the
// first, `<path>#<n>` for the others.
static std::string inclusion_key(const std::string& path, unsigned n) {
    return n <= 1 ? path : path + "#" + std::to_string(n);
}

static std::pair<std::string, unsigned> decode_inclusion_key(const std::string& key) {
    const size_t hash = key.rfind('#');
    if (hash == std::string::npos || hash + 1 >= key.size()) return {key, 1u};
    for (size_t i = hash + 1; i < key.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(key[i]))) return {key, 1u};
    return {key.substr(0, hash), static_cast<unsigned>(std::stoul(key.substr(hash + 1)))};
}

// Where inclusion n's copy lives relative to <split_dir>/include.
static std::string inclusion_mirror_relpath(const std::string& rel, unsigned n) {
    return n <= 1 ? rel : (fs::path("_inclusion") / std::to_string(n) / rel).string();
}

// Headers the current unit splits per inclusion, and the #include lines of the unit that
// read each file: (line in the unit, the name as written), in the order they were processed.
static std::set<std::string> g_pair_headers;
static std::map<std::string, std::vector<std::pair<unsigned, std::string>>> g_inclusion_sites;

// One #include line of the unit's preamble to point at a later inclusion's copy: the name as
// written, which textual occurrence of it in the unit, and what to name instead.
struct PreambleIncludeRewrite {
    std::string unit;
    std::string spelled;
    unsigned occurrence;
    std::string replacement;
};
static std::vector<PreambleIncludeRewrite> g_preamble_include_rewrites;

// Whether a line is `#include "spelled"` or `#include <spelled>`.
static bool is_include_of(const std::string& line, const std::string& spelled) {
    const size_t hash = line.find_first_not_of(" \t");
    if (hash == std::string::npos || line[hash] != '#') return false;
    const size_t kw = line.find_first_not_of(" \t", hash + 1);
    if (kw == std::string::npos || line.compare(kw, 7, "include") != 0) return false;
    return line.find("\"" + spelled + "\"", kw + 7) != std::string::npos ||
           line.find("<" + spelled + ">", kw + 7) != std::string::npos;
}

// The number of lines before `line` (1-based) that include `spelled`.
static unsigned include_occurrence_before(const std::string& source, unsigned line,
                                          const std::string& spelled) {
    unsigned count = 0, current = 1;
    size_t pos = 0;
    while (pos < source.size() && current < line) {
        size_t eol = source.find('\n', pos);
        if (eol == std::string::npos) eol = source.size();
        if (is_include_of(source.substr(pos, eol - pos), spelled)) ++count;
        pos = eol + 1;
        ++current;
    }
    return count;
}

static std::string apply_preamble_include_rewrites(const std::string& preamble,
                                                   const std::string& unit) {
    std::string text = preamble;
    for (const auto& rw : g_preamble_include_rewrites) {
        if (rw.unit != unit) continue;
        std::string rebuilt;
        rebuilt.reserve(text.size());
        unsigned seen = 0;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t eol = text.find('\n', pos);
            const bool last = (eol == std::string::npos);
            if (last) eol = text.size();
            std::string line = text.substr(pos, eol - pos);
            if (is_include_of(line, rw.spelled) && seen++ == rw.occurrence) {
                const size_t kw = line.find("include");
                line = line.substr(0, kw + 7) + " \"" + rw.replacement + "\"";
            }
            rebuilt += line;
            if (last) break;
            rebuilt += '\n';
            pos = eol + 1;
        }
        text.swap(rebuilt);
    }
    return text;
}

struct VisitorData {
    CXTranslationUnit tu;
    const std::set<std::string>* wanted;   // files whose functions to record
    HarvestMap* harvest;
    VarHarvestMap* variables;
};

static std::string blank_code_noise(const std::string& text);
static bool is_included_file(const std::string& path);
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
    if (const char* dump = std::getenv("CPP_SPLITTER_DUMP_HARVEST")) {
        if (file.find(dump) != std::string::npos) {
            unsigned line = 0;
            clang_getFileLocation(clang_getCursorLocation(cursor), nullptr, &line, nullptr, nullptr);
            fprintf(stderr, "[harvest-var] line=%-5u %s\n", line,
                    cx_to_string(clang_getCursorSpelling(cursor)).c_str());
        }
    }

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
    info.sloc_raw = raw_sloc_offset(clang_getCursorLocation(cursor));
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
    bool innermost = true;
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
            if (uk == CXCursor_Namespace) {
                if (innermost) ++info.unnamed_ns_depth;
                else info.in_unnamed_ns = true;
            }
        } else {
            innermost = false;
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
    info.usr = cx_to_string(clang_getCursorUSR(cursor));
    // The declaration that would replace a moved variable names its type. If that type is
    // one another translation unit cannot name -- a class in an unnamed namespace, or a
    // local or unnamed class -- the variable cannot be defined anywhere but here, and the
    // pieces are other translation units. OpenCV's `static AllocatorStatistics
    // allocator_stats;`, of a class the header defines in an unnamed namespace.
    {
        CXType t = clang_getCanonicalType(clang_getCursorType(cursor));
        while (t.kind == CXType_Pointer || t.kind == CXType_LValueReference ||
               t.kind == CXType_RValueReference)
            t = clang_getPointeeType(t);
        while (t.kind == CXType_ConstantArray || t.kind == CXType_IncompleteArray)
            t = clang_getArrayElementType(t);
        CXCursor decl = clang_getTypeDeclaration(t);
        if (!clang_Cursor_isNull(decl) && clang_getCursorKind(decl) != CXCursor_NoDeclFound) {
            const CXLinkageKind lk = clang_getCursorLinkage(decl);
            if (lk == CXLinkage_Internal || lk == CXLinkage_NoLinkage)
                info.type_lacks_linkage = true;
            // An empty type carries no state, so one copy per piece is the same object for
            // every purpose: `static auto fun1 = [](auto&) {...};` in Boost.Spirit's x3
            // tests, a captureless lambda, whose closure type has no linkage. It stays in
            // the preamble, as it did before the rule; the rule is for a variable whose
            // copies would diverge.
            const long long size = clang_Type_getSizeOf(t);
            if (size >= 0 && size <= 1) info.is_empty_type = true;
            if (info.type_lacks_linkage && size >= 0 && size <= 1)
                info.type_lacks_linkage = false;
        }
    }
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

// Every function name the translation unit declares, with the USRs that carry it. System
// headers included: a static named `max` collides with std::max just as surely, and the
// rename that would follow is textual.
static std::map<std::string, std::set<std::string>> g_function_name_usrs;

static CXChildVisitResult visitor(CXCursor cursor, CXCursor /*parent*/, CXClientData data) {
    auto* vd = static_cast<VisitorData*>(data);

    CXSourceLocation loc = clang_getCursorLocation(cursor);
    {
        const CXCursorKind k = clang_getCursorKind(cursor);
        if (k == CXCursor_FunctionDecl || k == CXCursor_CXXMethod ||
            k == CXCursor_FunctionTemplate) {
            const std::string usr = cx_to_string(clang_getCursorUSR(cursor));
            if (!usr.empty())
                g_function_name_usrs[cx_to_string(clang_getCursorSpelling(cursor))].insert(usr);
        }
    }
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
    // Every cursor seen in a file to split says which inclusion of it is being read -- the
    // macro expansions of the preprocessing record included, which is how a fragment of a
    // function body with no declaration of its own gets its inclusions told apart.
    if (wanted_file) note_inclusion_base(cursor_filename, loc);

    CXCursorKind kind = clang_getCursorKind(cursor);

    bool is_function_def = false;
    // CXCursor_ConversionFunction is harvested, and it carries a known defect: see TODO/25
    // defect 3 and example/conversion-operator/.
    //
    // Leaving it out is a hole -- an unharvested definition is never moved out of the
    // preamble, so `context_frame::operator bool()` in Boost.Test's test_tools.ipp, an
    // out-of-line member with external linkage, is copied into every piece and `ld -r`
    // rejects the copies. Boost.Geometry's whole test suite fell back on that alone.
    //
    // Harvesting it clears the link failure and produces a program that compiles, links, and
    // then fails at run time: Boost.Geometry's area test reports "no argument provided for
    // parameter color_output" where the plain build passes. Keeping the definitions in the
    // header rather than emitting pieces for them does not help, so it is the *relocation*
    // into the definitions header that is wrong, not the piece. The cause is not understood.
    // is_conversion forces keep_in_header so that no piece is ever emitted for one.
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
        // A linkage specification too. OpenCV's C API is `CV_IMPL void cvMaxS(...) {...}`
        // with CV_IMPL expanding to `extern "C"`; a harvest that stopped here left every
        // such definition in the preamble verbatim, and every header piece emitted it.
        // libclang 13 reports the specification as CXCursor_UnexposedDecl (kind 1), not as
        // CXCursor_LinkageSpec; recursing into an unexposed declaration is harmless, since
        // only a definition among its children is acted on.
        if (kind == CXCursor_Namespace || kind == CXCursor_ClassDecl ||
            kind == CXCursor_StructDecl || kind == CXCursor_ClassTemplate ||
            kind == CXCursor_LinkageSpec || kind == CXCursor_UnexposedDecl) {
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
    info.sloc_raw = raw_sloc_offset(loc);
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
    {
        auto find_static = [](CXCursor c, CXCursor, CXClientData d) -> CXChildVisitResult {
            if (clang_getCursorKind(c) == CXCursor_VarDecl &&
                clang_Cursor_getStorageClass(c) == CX_SC_Static &&
                !clang_isConstQualifiedType(clang_getCanonicalType(clang_getCursorType(c)))) {
                *static_cast<bool*>(d) = true;
                return CXChildVisit_Break;
            }
            // Not into a lambda or a local class: their statics are theirs.
            const CXCursorKind k = clang_getCursorKind(c);
            if (k == CXCursor_LambdaExpr || k == CXCursor_ClassDecl || k == CXCursor_StructDecl)
                return CXChildVisit_Continue;
            return CXChildVisit_Recurse;
        };
        clang_visitChildren(cursor, find_static, &info.has_local_static);
    }
    {
        const CXCursor_ExceptionSpecificationKind es =
            static_cast<CXCursor_ExceptionSpecificationKind>(
                clang_getCursorExceptionSpecificationType(cursor));
        info.has_exception_spec = es != CXCursor_ExceptionSpecificationKind_None &&
                                  es != CXCursor_ExceptionSpecificationKind_Unevaluated &&
                                  es != CXCursor_ExceptionSpecificationKind_Uninstantiated &&
                                  es != CXCursor_ExceptionSpecificationKind_Unparsed;
        info.has_prior_declaration =
            !clang_equalCursors(clang_getCanonicalCursor(cursor), cursor);
    }

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
        if (pk == CXCursor_LinkageSpec || pk == CXCursor_UnexposedDecl) {
            // `extern "C"` is a declaration context but not a scope: it contributes no
            // qualifier, and the walk continues to whatever encloses it. libclang 13
            // reports a linkage specification as CXCursor_UnexposedDecl, not as the
            // CXCursor_LinkageSpec its header declares, so both are accepted. Whether it is
            // "C" rather than "C++" is read off the mangled name, which libclang gives and
            // the cursor does not: a C-linkage function mangles to its own name.
            const std::string mangled = cx_to_string(clang_Cursor_getMangling(cursor));
            if (mangled == info.name || mangled == "_" + info.name) info.c_linkage = true;
            parent_cursor = clang_getCursorSemanticParent(parent_cursor);
            continue;
        }
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
    CXFile extent_file = nullptr;
    clang_getFileLocation(start_loc, &extent_file, &start_line, nullptr, &s_off);
    clang_getFileLocation(end_loc, nullptr, &end_line, nullptr, &e_off);

    info.body = get_source_range_text(vd->tu, extent);

    // A special member defaulted or deleted out of line -- `T::~T() = default;`, OpenCV's
    // cuda_gpu_mat_nd.cpp -- has an extent that ends at the declarator. Cut out on that
    // extent, the preamble kept ` = default;` and the piece a declarator with nothing after
    // it. The extent is carried through the `;` so that the piece is the whole definition.
    if (extent_file) {
        size_t size = 0;
        const char* buf = clang_getFileContents(vd->tu, extent_file, &size);
        if (buf && e_off <= size) {
            size_t i = e_off;
            while (i < size && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r')) ++i;
            if (i < size && buf[i] == '=') {
                ++i;
                while (i < size && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n')) ++i;
                const std::string rest(buf + i, std::min<size_t>(8, size - i));
                size_t word = 0;
                if (rest.rfind("default", 0) == 0) word = 7;
                else if (rest.rfind("delete", 0) == 0) word = 6;
                if (word) {
                    size_t j = i + word;
                    while (j < size && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\n')) ++j;
                    if (j < size && buf[j] == ';') {
                        for (size_t k = e_off; k <= j; ++k)
                            if (buf[k] == '\n') ++end_line;
                        e_off = static_cast<unsigned>(j + 1);
                        info.body = std::string(buf + s_off, e_off - s_off);
                        info.is_defaulted_or_deleted = true;
                    }
                }
            }
        }
    }

    info.start_line = start_line;
    info.end_line = end_line;
    info.start_offset = s_off;
    info.end_offset = e_off;

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
    if (g_module_unit.interface && !is_included_file(fn.file)) {
        if (fn.is_inlined)        return "inline in a module interface: belongs in the BMI";
        if (fn.defined_in_class)  return "member defined in its class in a module interface: left in place";
    }
    if (fn.declare_only)        return "not emitted by this translation unit; declared only";
    if (fn.lacks_definition)    return "not emitted by this translation unit; refers to a function it does not define";
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
    if (fn.kept_with_variable)  return "uses a static no other translation unit can name; kept with it";
    if (fn.name_shared)         return "static whose name another function in the unit also has; a rename would reach both";
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
    // A last line that is a preprocessor directive cannot carry a token either: a declarator
    // written across `#ifdef ... #else ... #endif`, OpenCV's alloc.cpp `fastMalloc`, ends
    // on the `#endif`, and `#endif;` is a directive with an extra token, not a declaration.
    // The `;` on a line of its own terminates the declaration the tokens above it spell.
    const size_t first = decl.find_first_not_of(" \t", from);
    if (first != std::string::npos && decl[first] == '#')
        return decl + "\n;";
    return decl + ";";
}

// Declaration left in place of a removed free-function definition. It is emitted at the
// position the definition occupied, so it inherits the surrounding namespaces and any #if
// context and needs no wrapping of its own. Emitting it here rather than appending it to
// the end of the preamble also means it precedes every use the original file had: a
// function pointer initialised at namespace scope just below the definition would
// otherwise refer to a name that has not been declared yet.
// An exception specification the type has and the text does not spell was inherited from
// an earlier declaration, which the preamble already carries; a declaration written
// without it would be rejected, and one written with it is not needed.
static bool inherits_exception_spec(const FunctionInfo& fn, const std::string& sig) {
    if (!fn.has_exception_spec || !fn.has_prior_declaration) return false;
    const std::string blanked = blank_code_noise(sig);
    return !contains_decl_token(blanked, "noexcept") && !contains_decl_token(blanked, "throw");
}

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
    if (inherits_exception_spec(fn, sig))
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
    if (inherits_exception_spec(fn, sig))
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
        for (const auto& other : var.other_names)
            if (seen.insert(other).second)
                renames.emplace_back(other, make_static_mangled_name(unit_tag, other));
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
        // Never moved, so never renamed -- except one kept *with* a variable: it goes to the
        // definitions header rather than to a piece, and a `static` one has to be renamed so
        // the pieces can call it through the declaration left in its place.
        if (fn.keep_in_header && !fn.kept_with_variable) continue;

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

// Offset of the last whole-token occurrence of `name`: a variable's declarator, which the
// type's own tokens precede and an array bound or nothing follows.
static size_t find_variable_name(const std::string& blanked, const std::string& name) {
    if (name.empty()) return std::string::npos;
    size_t found = std::string::npos, pos = 0;
    while ((pos = blanked.find(name, pos)) != std::string::npos) {
        if (token_at(blanked, pos, name.size())) found = pos;
        pos += name.size();
    }
    return found;
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
// Whether a blanked extent is exactly one invocation, `NAME( ... )`, with an optional `;`.
static bool is_macro_invocation_text(const std::string& blanked) {
    size_t i = 0;
    while (i < blanked.size() && std::isspace(static_cast<unsigned char>(blanked[i]))) ++i;
    const size_t name = i;
    while (i < blanked.size() && is_ident_char(static_cast<unsigned char>(blanked[i]))) ++i;
    if (i == name || std::isdigit(static_cast<unsigned char>(blanked[name]))) return false;
    while (i < blanked.size() && std::isspace(static_cast<unsigned char>(blanked[i]))) ++i;
    if (i >= blanked.size() || blanked[i] != '(') return false;
    int depth = 0;
    for (; i < blanked.size(); ++i) {
        if (blanked[i] == '(') ++depth;
        else if (blanked[i] == ')' && --depth == 0) break;
    }
    if (depth != 0) return false;
    ++i;
    while (i < blanked.size() && std::isspace(static_cast<unsigned char>(blanked[i]))) ++i;
    if (i < blanked.size() && blanked[i] == ';') ++i;
    while (i < blanked.size() && std::isspace(static_cast<unsigned char>(blanked[i]))) ++i;
    return i == blanked.size();
}

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

// Whether a definition's text has a preprocessor directive on a line of its own.
static bool body_holds_directive(const std::string& body) {
    // On the text as written: blank_code_noise() blanks directives with the rest.
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        const size_t at = body.find_first_not_of(" \t", pos);
        if (at != std::string::npos && at < eol && body[at] == '#') return true;
        pos = eol + 1;
    }
    return false;
}

// The header definitions of the current translation unit that its copies may declare
// rather than define (TODO/48): not emitted by the unit, of a shape the split rules accept,
// and named nowhere else in the unit's own sources -- not in a template, whose references
// libclang cannot resolve (`this->has_trivial_copy_and_destroy()` in Boost.Function's
// function_n is a dependent call with no referenced declaration, and 1963 references to it
// went unresolved when the copies declared it), not in a definition kept for any reason,
// not in a namespace-scope initialiser. A candidate named only inside other candidates'
// bodies stays one: those bodies go too. Decided by the identifier tokens of every
// harvested file with the candidates' extents blanked, to a fixpoint, once per parse.
static std::set<std::string> g_declare_only;
// The candidates the fixpoint rejected: of the same shape, but named somewhere in the
// unit's sources, in a template as often as not. The name is no reason to keep the body in
// the copy: they are split like an emitted definition, the copy declaring and a piece
// defining, so an edit to the body recompiles that piece and not the PCH. TODO/49.
static std::set<std::string> g_split_unemitted;

static std::set<std::string> undefined_macros(const std::string& source);
static bool contains_decl_token(const std::string& blanked, const std::string& kw);
static bool is_stdlib_header(const std::string& abs_path);

static void identifiers_of(const std::string& blanked, std::set<std::string>& out) {
    size_t i = 0;
    while (i < blanked.size()) {
        const unsigned char c = static_cast<unsigned char>(blanked[i]);
        if (std::isalpha(c) || c == '_') {
            size_t j = i + 1;
            while (j < blanked.size() && is_ident_char(static_cast<unsigned char>(blanked[j]))) ++j;
            out.insert(blanked.substr(i, j - i));
            i = j;
        } else if (std::isdigit(c)) {
            size_t j = i + 1;
            while (j < blanked.size() && is_ident_char(static_cast<unsigned char>(blanked[j]))) ++j;
            i = j;
        } else {
            ++i;
        }
    }
}

static void declare_only_candidates(const HarvestMap& harvest,
                                    const std::vector<std::string>& included,
                                    const std::string& main_file,
                                    const std::set<std::string>& emitted) {
    g_declare_only.clear();
    g_split_unemitted.clear();
    struct Candidate { std::string file, name; unsigned start, end; };
    std::vector<Candidate> candidates;
    for (const auto& entry : harvest) {
        if (entry.first == main_file) continue;
        const std::set<std::string> undeffed = undefined_macros(read_file(entry.first));
        for (const auto& fn : entry.second) {
            if (fn.usr.empty() || emitted.count(fn.usr)) continue;
            if (!fn.is_inlined || !fn.external_linkage || fn.is_static || fn.in_unnamed_ns ||
                fn.is_template || fn.in_class_template || fn.in_anonymous_class ||
                fn.is_virtual || fn.is_ctor_or_dtor || fn.is_conversion || fn.is_constexpr ||
                fn.is_specialization || fn.is_defaulted_or_deleted || fn.shares_extent ||
                fn.name == "main")
                continue;
            // Called without being named: operators by their syntax -- Boost's
            // `atomic_count::operator--` went unresolved 513 times -- begin()/end() by a
            // range for, get() by a structured binding, the coroutine hooks by co_await.
            static const char* const implicit_names[] = {
                "begin", "end", "get", "await_ready", "await_suspend", "await_resume",
                "get_return_object", "initial_suspend", "final_suspend",
                "unhandled_exception", "return_void", "return_value", "yield_value"};
            if (fn.name.rfind("operator", 0) == 0) continue;
            bool implicit = false;
            for (const char* n : implicit_names) if (fn.name == n) implicit = true;
            if (implicit) continue;
            if (body_holds_directive(fn.body)) continue;
            const std::string blanked = blank_code_noise(fn.body);
            const size_t open = blanked.find('{');
            if (open == std::string::npos) continue;   // no body of its own
            if (contains_decl_token(blanked.substr(0, open), "auto")) continue;
            bool uses_undeffed = false;
            for (const auto& m : undeffed)
                if (contains_decl_token(blanked, m)) { uses_undeffed = true; break; }
            if (uses_undeffed) continue;
            g_declare_only.insert(fn.usr);
            candidates.push_back({entry.first, fn.name, fn.start_offset, fn.end_offset});
        }
    }
    if (candidates.empty()) return;

    // Identifiers of every file the unit read outside the system headers -- a macro in a
    // header with no definition of its own can name a candidate too -- with the candidates'
    // own extents blanked out; a rejected candidate's identifiers join them.
    std::set<std::string> mentioned;
    std::map<std::string, std::string> texts;
    for (const auto& entry : harvest) texts[entry.first] = read_file(entry.first);
    for (const auto& inc : included)
        if (!texts.count(inc) && !is_stdlib_header(inc)) texts[inc] = read_file(inc);
    if (!texts.count(main_file)) texts[main_file] = read_file(main_file);
    for (auto& t : texts) {
        std::string blanked = blank_code_noise(t.second);
        for (const auto& c : candidates)
            if (c.file == t.first && c.end <= blanked.size())
                std::fill(blanked.begin() + c.start, blanked.begin() + c.end, ' ');
        identifiers_of(blanked, mentioned);
    }
    std::vector<bool> rejected(candidates.size(), false);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (rejected[i] || !mentioned.count(candidates[i].name)) continue;
            rejected[i] = true;
            changed = true;
            const auto& t = texts[candidates[i].file];
            if (candidates[i].end <= t.size())
                identifiers_of(blank_code_noise(t.substr(candidates[i].start,
                                                          candidates[i].end - candidates[i].start)),
                               mentioned);
        }
    }
    // The rejected ones leave the set; the harvest entries carry the usr by position.
    size_t k = 0;
    for (const auto& entry : harvest) {
        if (entry.first == main_file) continue;
        for (const auto& fn : entry.second) {
            if (!g_declare_only.count(fn.usr)) continue;
            if (k < candidates.size() && candidates[k].start == fn.start_offset &&
                candidates[k].file == entry.first) {
                if (rejected[k]) {
                    g_declare_only.erase(fn.usr);
                    if (!g_lacks_definition.count(fn.usr)) g_split_unemitted.insert(fn.usr);
                }
                ++k;
            }
        }
    }
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

    // A macro that expands to exactly one definition. Its extent is a fragment of the
    // invocation; widening to the whole invocation makes it a unit that can be moved to the
    // definitions header, which is where a non-inline definition in a header has to go. An
    // out-of-line member is already declared by its class, so nothing left in the file
    // refers to the text; a free function is moved the way a group is, with a declaration
    // spelled from what libclang resolved left in its place (generate_preamble()). OpenCV's
    // arithm.simd.hpp writes `DEFINE_SIMD_U8(or, op_or)`: one external function, which every
    // piece emitted when it was left where it was.
    {
        const std::string blanked = blank_code_noise(source);
        for (auto& fn : functions) {
            if (fn.shares_extent || fn.defined_in_class) continue;
            if (extent_is_a_definition(fn, fn.body)) continue;

            unsigned start = fn.start_offset, end = fn.end_offset;
            // Already the whole invocation -- `PAIR_DEFINE(sc, aled, 3)` -- when no token
            // of the definition came from an argument: nothing to widen.
            if (is_macro_invocation_text(blanked.substr(start, end - start))) {
                fn.macro_invocation = true;
                continue;
            }
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

        // In a module interface unit an `inline` body is what importers instantiate or emit
        // themselves, so it stays in the interface, where the BMI carries it. A member
        // defined inside its class is not inline there (P1779) and could move, but moving it
        // rewrites the class; it stays until TODO/05's member emission covers it. TODO/43.
        if (g_module_unit.interface && !input_is_header &&
            (fn.is_inlined || fn.defined_in_class)) {
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

        // A static is renamed so that the pieces can share it, and the rename is applied
        // to every whole-identifier occurrence of the name. When another function anywhere
        // in the translation unit has the same unqualified name, some of those occurrences
        // are its: OpenCV's `static getCvtScaleAbsFunc()` dispatches through
        // `CV_CPU_DISPATCH(getCvtScaleAbsFunc, ...)`, whose expansion names
        // `cpu_baseline::getCvtScaleAbsFunc`, and renaming the token in the macro argument
        // left `no member named '__static_..._getCvtScaleAbsFunc' in namespace
        // 'cpu_baseline'`. Such a static stays in the preamble: internal linkage makes the
        // copy every piece then holds harmless.
        if (fn.is_static) {
            bool is_member = false;
            for (const auto& e : fn.scope_chain)
                if (e.kind == ScopeKind::Class) { is_member = true; break; }
            auto it = g_function_name_usrs.find(fn.name);
            if (!is_member && it != g_function_name_usrs.end() && it->second.size() > 1) {
                fn.name_shared = true;
                fn.keep_in_header = true;
                continue;
            }
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
        // A deduced return type has to stay visible to callers -- `auto`, `auto &`,
        // `const auto &`, `auto *`, `decltype(auto)`: any spelling with `auto` in it, since
        // the type is deduced whatever qualifiers or declarators surround the word. p4c's
        // IR writes `inline auto &getExpr()` and `inline const auto &getExpr() const`, and a
        // caller before the definition is an error. TODO/47.
        if (contains_decl_token(blank_code_noise(ret_src), "auto")) {
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

    // Only split what this translation unit actually emits; see collect_emitted(). What it
    // does not emit, of a shape the rules above would have split, is declared in the copy
    // and gets no piece (TODO/48) -- unless something in the unit names it, in which case
    // it is split as if emitted, a piece of its own that the copy declares (TODO/49): the
    // emit graph does not resolve a dependent call, so the name is the only evidence, and
    // a body kept in the copy is in the PCH and costs every piece on an edit. What the
    // rules above would have kept stays kept, unless it is not emitted either, in which
    // case the rules above already kept it.
    //
    // Not when the body holds a preprocessor directive: Boost's current_function.hpp
    // defines BOOST_CURRENT_FUNCTION inside the body of an inline function nothing calls,
    // and the declaration that replaced it took the macro away from every BOOST_TEST.
    if (input_is_header)
        for (auto& fn : functions) {
            if (fn.keep_in_header || fn.usr.empty()) continue;
            if (referenced.find(fn.usr) != referenced.end()) continue;
            if (g_split_unemitted.count(fn.usr)) continue;
            fn.keep_in_header = true;
            fn.declare_only = g_declare_only.count(fn.usr) != 0;
            fn.lacks_definition = !fn.declare_only && g_lacks_definition.count(fn.usr) != 0;
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

// A `static` variable no rule could move out of the preamble. Left there it is one object
// per piece and the link says nothing -- internal linkage -- so the program is wrong rather
// than the build: p4c's gc.cpp had `static bool done_init, started_init;` and
// `static char emergency_pool[16 * 1024];` duplicated into every piece, and the split
// irgenerator initialised the collector once per copy and aborted. Such a unit is declined
// (do_split()), with the variable named. TODO/47.
static std::string g_unmovable_static_variable;

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
            if (var.type_lacks_linkage) {
                // No other translation unit can name its type, so no `extern` can stand in
                // for it and no piece may hold a copy. It moves whole to the definitions
                // header, `static` and type definition included, with nothing left behind;
                // every function that references it goes there with it (TODO/44).
                var.anchors_users = true;
                var.move_out = true;
            } else {
                var.rename_and_move = true;
                var.move_out = true;
            }
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

        // A variable whose type no other translation unit can name: the whole declaration
        // moves, text as written, and the preamble gets nothing in its place -- the only
        // code that names it moves too. Several declarators in one such declaration move
        // together, since the text is one.
        if (group.anchors_users) {
            group.replacement.clear();
            kept.push_back(std::move(group));
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
                    // A static of this shape is renamed like any other -- every use and the
                    // moved definition get the mangled name through apply_static_renames()
                    // -- so the declaration left here has to spell it too. OpenCV's
                    // `static struct LABLUVLUT_s16_t {...} LABLUVLUTs16` kept the original
                    // name here and no use could find it.
                    const std::string declared =
                        group.rename_and_move ? make_static_mangled_name(unit_tag, group.name)
                                              : group.name;
                    group.replacement = ";\nextern " + group.type_spelling + " " +
                                        declared + ";";
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
                //
                // The declaration left behind is the source's own declarator with the name
                // mangled, which carries an array bound or a pointer as written --
                // `static char emergency_pool[16 * 1024]` -- and the pretty-printed type
                // only when the text gives no declarator. TODO/47.
                const std::string mangled = make_static_mangled_name(unit_tag, group.name);
                std::string decl;
                // `extern auto x;` is not a declaration; a deduced type has to be spelled.
                if (!head.empty() && !contains_decl_token(head_blanked, "auto")) {
                    std::string h = strip_decl_specifier(head, "static");
                    const size_t name_pos = find_variable_name(blank_code_noise(h), group.name);
                    if (name_pos != std::string::npos) {
                        h.replace(name_pos, group.name.size(), mangled);
                        decl = h;
                    }
                }
                if (decl.empty() && type_spelling_is_simple(group.type_spelling))
                    decl = group.type_spelling + " " + mangled;
                if (!decl.empty()) {
                    group.text = strip_decl_specifier(group.text, "static");
                    group.replacement = terminate_declaration("extern " + decl);
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
            //
            // Several `static` declarators are renamed together, each declared under its
            // mangled name, the text moved with `static` off -- apply_static_renames()
            // reaches every name through `other_names`. TODO/47.
            for (size_t k = i; k < j && usable_group; ++k) {
                if (usable[k].is_member || !type_spelling_is_simple(usable[k].type_spelling) ||
                    usable[k].rename_and_move != group.rename_and_move)
                    usable_group = false;
            }
            if (usable_group) {
                group.replacement.clear();
                for (size_t k = i; k < j; ++k) {
                    const std::string declared =
                        group.rename_and_move ? make_static_mangled_name(unit_tag, usable[k].name)
                                              : usable[k].name;
                    group.replacement +=
                        "extern " + usable[k].type_spelling + " " + declared + ";\n";
                    if (k > i) group.other_names.push_back(usable[k].name);
                }
                if (group.rename_and_move)
                    group.text = strip_decl_specifier(group.text, "static");
            }
        }

        // A static that cannot be moved must not stay either: every piece would get one --
        // unless its type is empty, in which case every copy is the same object.
        if (!usable_group && group.rename_and_move && !group.is_empty_type &&
            g_unmovable_static_variable.empty())
            g_unmovable_static_variable = group.name;

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
            // A `(` opens a direct-initialiser -- `int x(5)` -- unless what follows it is
            // `*` or `&`, which makes it a declarator: p4c's
            // `static size_t (*mod_hashsize[])(size_t x) = {...}`. TODO/47.
            // The parameter list that follows such a declarator's `)` is one as well.
            bool declarator_paren = false;
            if (c == '(') {
                size_t j = i + 1;
                while (j < blanked.size() && blanked[j] == ' ') ++j;
                declarator_paren = j < blanked.size() && (blanked[j] == '*' || blanked[j] == '&');
                size_t k = i;
                while (k > 0 && blanked[k - 1] == ' ') --k;
                if (k > 0 && blanked[k - 1] == ')') declarator_paren = true;
            }
            if (c == '=' || (c == '(' && !declarator_paren) || c == '{' || c == ';')
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
static size_t conditionals_closed_by(const std::string& text);

// Set by generate_preamble() when a macro group it moved to the definitions header names a
// macro the file undefines: re-expanded behind the whole preamble the invocation would not
// mean what it meant in place. p4c's visitor.cpp invokes IRNODE_ALL_SUBCLASSES(
// DEFINE_VISIT_FUNCTIONS) twice, each time under its own #define, with an #undef after.
// The unit's definitions piece then compiles a variant of the unit with the definitions
// left in place (split_unit()), as a pair header's inclusion does. TODO/47.
static bool g_definitions_need_variant = false;

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
        // Every function whose extent was folded into this range -- one macro invocation
        // expanding to several definitions. `fn` is null for such a range; this is what
        // remains known about it.
        std::vector<const FunctionInfo*> group;
    };
    std::vector<Range> ranges;
    for (const auto& fn : functions) {
        ranges.push_back({fn.start_offset, fn.end_offset, should_keep_in_header(fn), &fn, nullptr, {}});
    }
    // Only the variables that move are listed. One that stays is ordinary text between the
    // function extents and is copied through without an entry, which is what it already was.
    for (const auto& var : variables) {
        ranges.push_back({var.start_offset, var.end_offset, false, nullptr, &var, {}});
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
                if (prev.fn) prev.group.push_back(prev.fn);
                if (r.fn) prev.group.push_back(r.fn);
                if (r.var || prev.var) prev.group.clear();   // a variable in it: not handled
                prev.keep = true;
                prev.fn = nullptr;
                prev.var = nullptr;
                continue;
            }
            merged.push_back(r);
        }
        ranges.swap(merged);
    }

    // A group's union of extents can be a fragment of the invocation -- `DEFINE_APPLY)` of
    // `ALL_OPS(DEFINE_APPLY)`, when every definition's first token came from the argument.
    // Left in place it reproduces the source; moved, it would leave `ALL_OPS(` behind. The
    // range is widened to the whole invocation, as a single macro-produced definition's is
    // in prepare_functions(). TODO/47.
    {
        const std::string blanked_source = blank_code_noise(source);
        for (size_t i = 0; i < ranges.size(); ++i) {
            Range& r = ranges[i];
            if (r.group.empty() || r.end > source.size()) continue;
            if (is_macro_invocation_text(blanked_source.substr(r.start, r.end - r.start)))
                continue;
            unsigned start = r.start, end = r.end;
            if (!widen_to_macro_invocation(blanked_source, start, end)) continue;
            const bool clear_before = i == 0 || ranges[i - 1].end <= start;
            const bool clear_after = i + 1 == ranges.size() || end <= ranges[i + 1].start;
            if (clear_before && clear_after && end <= source.size()) {
                r.start = start;
                r.end = end;
            }
        }
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

    // The declaration that stands in for a definition taken out of the preamble: inside
    // the class for a member, otherwise right here, where the definition used to be.
    auto declare_in_place = [&](const FunctionInfo& fn) {
        ensure_newline();
        if (!fn.member_decl.empty()) {
            preamble += terminate_declaration(apply_static_renames(fn.member_decl, renames));
            return;
        }
        // Only a renamed function needs its declaration here. Its name changed, so a use
        // in a namespace-scope initialiser retained in the preamble -- the `fn_ptr =
        // &impl;` idiom -- refers to a name nothing has declared yet, and a declaration
        // appended at the end of the preamble comes far too late.
        std::string decl = generate_forward_decl_inplace(fn, stem);
        if (decl.empty()) return;
        // The split definition is hoisted out of any unnamed namespace, so the declaration
        // has to leave it too, or the two get different linkage and the reference goes
        // unresolved. Closing and reopening the unnamed namespace puts the declaration in
        // the definition's scope while still keeping it ahead of every use.
        for (unsigned i = 0; i < fn.unnamed_ns_depth; ++i) preamble += "}\n";
        preamble += decl + "\n";
        for (unsigned i = 0; i < fn.unnamed_ns_depth; ++i) preamble += "namespace {\n";
    };

    unsigned pos = 0;
    for (const auto& r : ranges) {
        if (r.start > pos) {
            preamble += apply_static_renames(source.substr(pos, r.start - pos), renames);
        }
        if (r.keep && r.fn && r.fn->declare_only) {
            // Not emitted by this unit: declared, as it would be if it were split, and no
            // piece written. The copy stays what it is under an edit to the body (TODO/48).
            declare_in_place(*r.fn);
            {
                const std::string blanked = blank_code_noise(r.fn->body);
                const size_t decl_end = definition_decl_end(blanked);
                if (decl_end != std::string::npos && decl_end < r.fn->body.size()) {
                    const size_t whole = conditionals_closed_by(r.fn->body);
                    const size_t in_decl = conditionals_closed_by(r.fn->body.substr(0, decl_end));
                    for (size_t ci = in_decl; ci < whole; ++ci) preamble += "#endif\n";
                }
            }
        } else if (r.keep) {
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
            // A folded group -- one macro invocation, several definitions -- gets the same
            // treatment as one definition when every function in it may exist in only one
            // object and none is a class member: the invocation moves whole to the
            // definitions header, and a declaration for each function takes its place, since
            // no declarator in the source covers the group and there is nothing to leave
            // behind otherwise. A group mixing inline and non-inline functions is left where
            // it is: a piece that calls an inline one needs its definition, and the
            // definitions object does not emit an inline function nothing there uses.
            // OpenCV's DEFINE_SIMD_ALL(recip, ...) expands to eight external functions and
            // was emitted by every piece of the unit.
            // One macro invocation, one free function: moved like a group of one, since no
            // declarator in the source covers it either. A member keeps the branch below.
            std::vector<const FunctionInfo*> group = r.group;
            if (group.empty() && r.fn && r.fn->macro_invocation && r.fn->member_decl.empty()) {
                bool member = false;
                for (const auto& sc : r.fn->scope_chain)
                    if (sc.kind == ScopeKind::Class) member = true;
                if (!member) group.push_back(r.fn);
            }
            bool group_moves = false;
            if (definitions && !group.empty()) {
                group_moves = true;
                for (const FunctionInfo* g : group) {
                    if (has_vague_linkage(*g) || !g->member_decl.empty() || g->in_class_template ||
                        g->in_anonymous_class || g->is_static || g->in_unnamed_ns)
                        group_moves = false;
                    // An out-of-line member is fine: the invocation spells its qualified
                    // name and the class already declares it, so nothing is left behind.
                    // p4c's dbprint-expression.cpp defines IR::UPlus::dbprint and a dozen
                    // more through ALL_UNARY_OPS(UNOP_DBPRINT); kept in the preamble they
                    // were in every piece. A member written inside its class is not.
                    if (g->defined_in_class) group_moves = false;
                }
            }
            if (group_moves) {
                const std::string group_blanked = blank_code_noise(text);
                for (const auto& m : undefined_macros(source))
                    if (contains_decl_token(group_blanked, m)) g_definitions_need_variant = true;
                *definitions += wrap_in_namespaces(text, group.front()->scope_chain);
                *definitions += "\n";
                for (const FunctionInfo* g : group) {
                    // The source extent is the macro invocation, so no declarator can be
                    // read out of it; the declaration is spelled from what libclang
                    // resolved: the result type and the display name, which is the name
                    // with its parameter types. Default arguments are not carried; a piece
                    // relying on one fails to compile and the unit falls back, which is
                    // the visible outcome rather than the silent one. A member needs none:
                    // its class declares it.
                    bool member = false;
                    for (const auto& sc : g->scope_chain)
                        if (sc.kind == ScopeKind::Class) member = true;
                    if (member) continue;
                    std::string decl = generate_forward_decl_inplace(*g, stem);
                    if (decl.empty() && !g->return_type.empty() && !g->qualified_name.empty())
                        decl = g->return_type + " " + g->qualified_name + ";";
                    if (!decl.empty()) preamble += decl + "\n";
                }
            } else if (definitions && r.fn &&
                       (r.fn->kept_with_variable ||
                        (!has_vague_linkage(*r.fn) &&
                         (r.fn->macro_invocation || extent_is_a_definition(*r.fn, text))))) {
                // A definition that needs a macro the file retracts cannot be re-expanded
                // behind the whole preamble, and it cannot stay here as `inline` when it is
                // a virtual member: the class's key function made inline in one translation
                // unit is a vtable no object emits -- flex's P4Lexer::yylex, written under
                // YY_DECL, and `undefined symbol: vtable for P4::P4Lexer`. The definitions
                // piece then compiles a variant of the unit with the definition in place
                // (split_unit()), where the macro still means what it meant. TODO/47.
                if (r.fn->uses_undefined_macro) g_definitions_need_variant = true;
                // Kept, but it may exist in only one object. Out of the shared preamble it
                // goes; a member is already declared by its class, and a free function gets
                // a declaration left where its body was. The text is lifted out of whatever
                // namespaces enclosed it, so they have to be reopened around it.
                //
                // A function kept *with* a variable no other translation unit can name goes
                // here whatever its linkage. A `static` one loses the keyword: it was renamed
                // with the rest (build_static_rename_map()), and the pieces call it through
                // the declaration left below, which names the mangled form.
                if (r.fn->kept_with_variable && r.fn->is_static)
                    text = strip_decl_specifier(text, "static");
                // Under the conditionals the definition was written under, as a piece
                // replays them: a declarator written across `#ifdef ... #else ... #endif`
                // starts inside the first branch, and its text carries the `#else` and the
                // `#endif` without the `#ifdef` -- flex's `yyFlexLexer::LexerInput`, a
                // virtual member moved here. TODO/47.
                {
                    std::string conditioned;
                    for (const auto& c : r.fn->conditionals) conditioned += c + "\n";
                    conditioned += text;
                    const size_t closed = conditionals_closed_by(text);
                    const size_t to_close = r.fn->conditionals.size() > closed
                                                ? r.fn->conditionals.size() - closed : 0;
                    for (size_t ci = 0; ci < to_close; ++ci) conditioned += "\n#endif";
                    *definitions += wrap_in_namespaces(conditioned, r.fn->scope_chain);
                    // And the preamble, which loses the whole text, closes what that text
                    // closed for it.
                    for (size_t ci = 0; ci < closed; ++ci) preamble += "#endif\n";
                }
                *definitions += "\n";
                if (r.fn->member_decl.empty()) {
                    std::string decl = generate_forward_decl_inplace(*r.fn, stem);
                    if (!decl.empty()) preamble += decl + "\n";
                }
            } else {
                // Not in a definitions variant (no sink): there the definition exists once
                // and must be emitted as written -- `inline` on P4Lexer::yylex made it a
                // definition no object held.
                if (definitions && r.fn && r.fn->uses_undefined_macro && !has_vague_linkage(*r.fn)) {
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
                //
                // And `__attribute__((used))`, for the same reason a header's split-out
                // function carries it: an inline variable is emitted only in a translation
                // unit that uses it, and the pieces of the unit that defines it may use it
                // nowhere -- p4c's indent.cpp defines `int indent_t::tabsz = 2;` and only
                // log.cpp reads it, through indent.h's `operator<<`. Every piece then
                // emits a weak copy and the linker keeps one (TODO/47).
                //
                // Not on a template -- a member of a class template, which is instantiated
                // where it is used and takes no attribute in front of `template` -- nor on a
                // declaration a macro produces, whose text cannot be told apart from one:
                // Boost.Random's BOOST_RANDOM_MT_DEFINE_CONSTANT(UIntType, default_seed)
                // expands to exactly that template.
                std::string text = apply_static_renames(r.var->text, renames);
                const std::string blanked = blank_code_noise(text);
                if (!contains_decl_token(blanked, "inline")) {
                    const std::string lead = trim_ws(blanked);
                    const bool is_template = lead.rfind("template", 0) == 0;
                    text.insert(0, is_template || is_macro_invocation_text(lead)
                                       ? "inline "
                                       : "__attribute__((used)) inline ");
                }
                preamble += text;
            } else if (!definitions) {
                // No definitions header to move it to: leave it exactly where it was.
                preamble += apply_static_renames(r.var->text, renames);
            } else {
                *definitions += wrap_in_namespaces(
                    apply_static_renames(r.var->text, renames), r.var->scope_chain);
                *definitions += "\n";
                if (!r.var->replacement.empty()) {
                    // Hoisted out of its unnamed namespaces: the declaration goes where the
                    // definition went, the enclosing named scope, and the namespaces are
                    // closed and reopened around it so every use still follows it.
                    for (unsigned i = 0; i < r.var->unnamed_ns_depth; ++i) preamble += "}\n";
                    preamble += r.var->replacement + "\n";
                    for (unsigned i = 0; i < r.var->unnamed_ns_depth; ++i) preamble += "namespace {\n";
                }
            }
        } else if (r.fn) {
            // The definition moved to a split file, so a declaration has to take its place.
            declare_in_place(*r.fn);
            // The body taken out may close conditionals it did not open: p4c's
            // parseInput.cpp writes `#ifdef SUPPORT_P4_14 T f(a, b) { #else T f(a) { #endif`
            // and the body after -- the `#endif` is past the `{`, so the declaration left
            // behind (the declarator up to the `{`) loses it and the `#ifdef` stays open.
            // What the removed part closed is closed again here; the piece does the same
            // for what it replays (conditionals_closed_by()). TODO/47.
            {
                // Counted on the whole extent less what the kept declarator itself closes:
                // a declarator written across `#ifdef ... #else ... #endif` keeps its
                // `#endif`, and a constructor whose initialiser list is under `#if` opens
                // and closes inside the part removed.
                const std::string blanked = blank_code_noise(r.fn->body);
                const size_t decl_end = definition_decl_end(blanked);
                if (decl_end != std::string::npos && decl_end < r.fn->body.size()) {
                    const size_t whole = conditionals_closed_by(r.fn->body);
                    const size_t in_decl = conditionals_closed_by(r.fn->body.substr(0, decl_end));
                    for (size_t ci = in_decl; ci < whole; ++ci) preamble += "#endif\n";
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

    // A later inclusion of a pair header names its own copy. TODO/44 (B).
    return apply_preamble_include_rewrites(preamble, source_path);
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
                                                    bool tipi_compiler_driver_in_use,
                                                    bool verbose,
                                                    std::ostream& out = std::cout) {
    std::vector<CompileResult> results(jobs.size());
    std::mutex output_mtx;
    std::vector<std::thread> threads;
    std::atomic<size_t> next_job{0};

    unsigned num_threads = std::min(static_cast<unsigned>(jobs.size()), get_parallelism());
    const char* CMAKE_BUILD_PARALLEL_LEVEL = std::getenv("CMAKE_BUILD_PARALLEL_LEVEL"); 
    if (CMAKE_BUILD_PARALLEL_LEVEL) {
      num_threads = std::atoi(CMAKE_BUILD_PARALLEL_LEVEL);
    } 

    // remote execution just pass it all
    if (tipi_compiler_driver_in_use) { num_threads = jobs.size(); }

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
    // The last four are the module interface unit spellings the compilers accept. TODO/43.
    static const char* exts[] = {".cpp", ".cc", ".cxx", ".C", ".c++", ".cp", ".c",
                                 ".cppm", ".ixx", ".cxxm", ".c++m"};
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

static ModuleUnit detect_module_unit(const std::string& source) {
    ModuleUnit mu;
    const std::string blanked = blank_code_noise(source);
    size_t pos = 0;
    size_t gmf_start = std::string::npos;
    while (pos < blanked.size()) {
        size_t eol = blanked.find('\n', pos);
        if (eol == std::string::npos) eol = blanked.size();
        const std::string line = trim_ws(blanked.substr(pos, eol - pos));
        if (line == "module;") {
            gmf_start = eol < blanked.size() ? eol + 1 : eol;
        } else if (line.rfind("export module ", 0) == 0 || line.rfind("module ", 0) == 0) {
            const bool exported = line.rfind("export ", 0) == 0;
            std::string rest = trim_ws(line.substr(exported ? 14 : 7));
            const size_t semi = rest.find(';');
            if (semi == std::string::npos) break;
            rest = trim_ws(rest.substr(0, semi));
            // A partition (`M:part`) is not handled; treat it as an implementation unit and
            // compile it whole.
            if (rest.empty() || rest.find(':') != std::string::npos) break;
            mu.name = rest;
            if (exported) mu.interface = true; else mu.implementation = true;
            if (gmf_start != std::string::npos && gmf_start <= pos)
                mu.gmf = source.substr(gmf_start, pos - gmf_start);
            break;
        } else if (!line.empty() && line[0] != '#') {
            // Real code before any module declaration: not a module unit.
            break;
        }
        pos = eol + 1;
    }
    return mu;
}

// The unit's own `import` declarations, as written. An import inside a precompiled
// preamble does not make the module's declarations visible to the piece that includes it
// (measured, clang 21), so every piece restates them ahead of the preamble.
static std::vector<std::string> unit_import_lines(const std::string& source) {
    std::vector<std::string> imports;
    const std::string blanked = blank_code_noise(source);
    size_t pos = 0;
    while (pos < blanked.size()) {
        size_t eol = blanked.find('\n', pos);
        if (eol == std::string::npos) eol = blanked.size();
        const std::string line = trim_ws(blanked.substr(pos, eol - pos));
        if (line.rfind("import ", 0) == 0 || line.rfind("export import ", 0) == 0) {
            if (line.find(';') != std::string::npos)
                imports.push_back(trim_ws(source.substr(pos, eol - pos)));
        }
        pos = eol + 1;
    }
    return imports;
}

// The module syntax of an interface unit blanked out, every other byte where it was.
//
// libclang reports an `export` declaration as CXCursor_UnexposedDecl and does not visit its
// children (clang 21), so an exported definition is invisible to the harvest. Given a copy
// with `module;`, the module declaration, each `export` keyword and the braces of an
// `export { ... }` block replaced by spaces, libclang parses an ordinary translation unit
// whose declarations sit at the source's own offsets -- which is all the harvest records.
// The rewrite works on the real text, where `export` precedes each moved definition's
// extent and so stays. TODO/43.
static std::string blank_module_syntax(const std::string& source) {
    std::string out = source;
    const std::string blanked = blank_code_noise(source);
    auto blank_range = [&](size_t from, size_t to) {
        for (size_t i = from; i < to && i < out.size(); ++i)
            if (out[i] != '\n') out[i] = ' ';
    };
    auto is_ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    // Whole lines first: `module;` and the module declaration.
    size_t pos = 0;
    while (pos < blanked.size()) {
        size_t eol = blanked.find('\n', pos);
        if (eol == std::string::npos) eol = blanked.size();
        const std::string line = trim_ws(blanked.substr(pos, eol - pos));
        if (line == "module;" || line.rfind("export module ", 0) == 0 ||
            (line.rfind("module ", 0) == 0 && line.find(';') != std::string::npos))
            blank_range(pos, eol);
        pos = eol + 1;
    }
    // Then every `export` keyword, and the braces of an `export {` block.
    size_t at = 0;
    while ((at = blanked.find("export", at)) != std::string::npos) {
        const bool word = (at == 0 || !is_ident(blanked[at - 1])) &&
                          (at + 6 >= blanked.size() || !is_ident(blanked[at + 6]));
        if (!word) { at += 6; continue; }
        blank_range(at, at + 6);
        size_t after = at + 6;
        while (after < blanked.size() && std::isspace((unsigned char)blanked[after])) ++after;
        if (after < blanked.size() && blanked[after] == '{') {
            int depth = 0;
            for (size_t i = after; i < blanked.size(); ++i) {
                if (blanked[i] == '{') ++depth;
                else if (blanked[i] == '}' && --depth == 0) { blank_range(i, i + 1); break; }
            }
            blank_range(after, after + 1);
        }
        at = after;
    }
    return out;
}

// Whether a file on disk is a module unit -- a header candidate it must never be.
static bool is_module_unit_file(const std::string& path) {
    static std::map<std::string, bool> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    std::error_code ec;
    bool result = false;
    if (fs::is_regular_file(path, ec)) {
        const ModuleUnit mu = detect_module_unit(read_file(path));
        result = mu.interface || mu.implementation;
    }
    cache[path] = result;
    return result;
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

// Precompile the preamble that every split piece includes.
//
// This is the PCH that makes the *split output* affordable to compile, and it must not be
// removed. Each piece begins with `#include "<tag>_preamble.h"`, and the preamble is the whole
// translation unit with the function bodies carved out -- every include, class, template and
// typedef the unit had. Compiling that once per piece would multiply the unit's parse cost by
// the number of pieces, which on a Boost.Geometry test is a few hundred.
//
// It is fed to the compiler by placement, not by a flag: clang and gcc both look for
// `<header>.gch/` beside a header they are told to include, so nothing appears on the piece's
// command line. The directory holds one entry per content hash of the preamble, so a preamble
// that changes does not silently reuse the previous PCH.
//
// Do not confuse this with either of the two libclang-side mechanisms in split_unit(), which
// speed up the *splitter's own parse* and do nothing for the pieces: the prefix PCH built by
// build_libclang_pch(), and the CXTranslationUnit_PrecompiledPreamble parse flag. Optimising
// the splitter means touching those; touching this one only makes the build slower. See
// TODO/29, where a measurement that mixed the two nearly removed the wrong thing.
// The flag that makes a piece's compile load the PCH: `-include <preamble>`. clang consults
// `<header>.gch/` only for a header named by -include, never for an #include directive in
// the source -- GCC does both -- so with the preamble reached through the piece's own
// `#include "<tag>_preamble.h"` alone the PCH was built and never read, and every piece of
// p4c's def_use.cpp parsed the 62 MB of IR headers again: 3.7s a piece against 7.1s for the
// whole unit, and 47 minutes for a build that takes 2 plain. The textual include that
// follows is then a no-op, the PCH carrying the file's pragma-once state. The header is
// read off the piece's first quoted include, which names the unit's preamble or, for a pair
// header's inclusion, its context preamble (TODO/44). TODO/47.
// Whether the compiler's PCH still matches the contents of everything it was built from:
// `<gch>.deps` holds one `<content hash> <path>` per prerequisite. By content, not by
// time: a header touched and not changed keeps the PCH, as it keeps the split (TODO/28).
static std::string file_content_hash(const std::string& path);
static bool gch_is_current(const std::string& gch_file) {
    std::ifstream ifs(gch_file + ".deps");
    if (!ifs.is_open()) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        const size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        if (file_content_hash(line.substr(space + 1)) != line.substr(0, space)) return false;
    }
    return true;
}

// The prerequisites a compiler wrote with -MD: everything after the colon, backslashes
// dropped. Spaces in paths are not handled, as rewrite_depfile() does not.
static std::vector<std::string> depfile_prerequisites(const std::string& dep_file) {
    std::vector<std::string> deps;
    const std::string contents = read_file(dep_file);
    const size_t colon = contents.find(':');
    if (colon == std::string::npos) return deps;
    std::istringstream tokens(contents.substr(colon + 1));
    std::string token;
    while (tokens >> token)
        if (token != "\\") deps.push_back(token);
    return deps;
}

// The header a piece includes first, when it sits at the root of the split directory: the
// unit's preamble, or a pair header's context preamble. Empty otherwise.
static std::string piece_context_header(const std::string& piece, const std::string& split_dir) {
    std::ifstream ifs(piece);
    std::string line;
    while (std::getline(ifs, line)) {
        const size_t hash = line.find_first_not_of(" \t");
        if (hash == std::string::npos || line[hash] != '#') continue;
        const size_t q1 = line.find('"'), q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
        if (line.find("include", hash) == std::string::npos || q2 == std::string::npos) return "";
        const std::string header = (fs::path(split_dir) / line.substr(q1 + 1, q2 - q1 - 1)).string();
        std::error_code ec;
        return fs::exists(header, ec) ? header : std::string();
    }
    return "";
}

static std::string pch_include_flag(const std::string& piece, const std::string& split_dir) {
    const std::string header = piece_context_header(piece, split_dir);
    if (header.empty()) return "";
    std::error_code ec;
    const std::string gch_dir = header + ".gch";
    if (!fs::is_directory(gch_dir, ec)) return "";
    for (const auto& entry : fs::directory_iterator(gch_dir, ec))
        if (entry.path().extension() == ".gch") return " -include " + shell_quote(header);
    return "";
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

    // Named after the preamble's own content, which does not change when a header it
    // includes does -- a mirrored copy re-split after an edit -- and clang refuses a PCH
    // whose inputs moved: `file ... has been modified since the precompiled header`. The
    // compiler is asked for the PCH's prerequisites (-MD) and they are checked the way
    // pch_is_current() checks the libclang one's. TODO/47.
    if (fs::exists(gch_file) && gch_is_current(gch_file)) {
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
    // Templates instantiated while the PCH is built rather than by every consumer: a
    // clang PCH otherwise carries its pending instantiations, and each piece performed
    // p4c's again -- 1.67s of PerformPendingInstantiations in a piece that compiled
    // nothing else, against 0.4s with them in the PCH; a real piece 1.97s -> 0.59s. The
    // PCH grows (61 -> 85 MB) and takes longer to build, once. clang 11 and later; GCC has
    // no such flag and no such cost. TODO/47.
    const std::string driver_name = fs::path(compiler_driver.empty() ? compiler : compiler_driver)
                                        .filename().string();
    if (driver_name.find("clang") != std::string::npos) cmd += " -fpch-instantiate-templates";
    for (const auto& f : flags)
        cmd += " " + shell_quote(f);
    if (!include_dir.empty())
        cmd += " -I" + shell_quote(include_dir);
    const std::string dep_file = gch_file + ".d";
    cmd += " -MD -MF " + shell_quote(dep_file) + " -MT " + shell_quote(gch_file);
    cmd += " -o " + shell_quote(gch_file) + " " + shell_quote(preamble_file);

    if (verbose) out << "  Building PCH: " << cmd << "\n";
    int ret = run_command_quiet(cmd);
    if (ret != 0) {
        if (verbose) out << "  PCH build failed (exit " << ret << "), continuing without PCH\n";
        fs::remove_all(gch_dir);
        return false;
    }
    {
        // `<content hash> <path>` per prerequisite, as gch_is_current() reads.
        std::ofstream deps(gch_file + ".deps");
        for (const auto& dep : depfile_prerequisites(dep_file))
            deps << file_content_hash(dep) << " " << dep << "\n";
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
        // A directive continued with a backslash is one logical line: cut in the middle of
        // Boost.Filesystem's `#define BOOST_UTF8_BEGIN_NAMESPACE \` the prefix defined the
        // macro empty and the parse read its continuation lines as three real namespaces
        // opened around the rest of the unit, and every `std::` name went unresolved.
        while (eol < blanked.size()) {
            size_t last = eol;
            while (last > pos && std::isspace(static_cast<unsigned char>(blanked[last - 1])) &&
                   blanked[last - 1] != '\n')
                --last;
            if (last == pos || blanked[last - 1] != '\\') break;
            eol = blanked.find('\n', eol + 1);
            if (eol == std::string::npos) eol = blanked.size();
        }
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
                                       std::ostream& out = std::cout,
                                       const std::string& quote_dir = "") {
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
    // The prefix is a copy of the unit's include block, written into the split directory.
    // A quoted include that the unit resolved against its own directory --
    // `#include "precomp.hpp"` beside modules/core/src/glob.cpp -- resolves against the
    // copy's directory here and is not found. -iquote gives it the original's directory, and
    // only for quoted includes, which is exactly the rule the compiler applied to the unit.
    if (!quote_dir.empty()) {
        pch_flags.push_back("-iquote");
        pch_flags.push_back(quote_dir);
    }

    std::vector<const char*> args;
    for (const auto& f : pch_flags) args.push_back(f.c_str());

    CXTranslationUnit tu = nullptr;
    // With a preprocessing record, so that the include directives of the prefix -- which the
    // main parse no longer reads from the source -- are still there for inclusions_of().
    CXErrorCode err = clang_parseTranslationUnit2(
        index, preamble_file.c_str(), args.data(),
        static_cast<int>(args.size()), nullptr, 0,
        CXTranslationUnit_ForSerialization | CXTranslationUnit_DetailedPreprocessingRecord, &tu);

    if (err != CXError_Success || !tu) {
        if (verbose) out << "  libclang PCH: parse failed (code: " << err << ")\n";
        clang_disposeIndex(index);
        fs::remove_all(pch_dir);
        return "";
    }

    // A PCH whose build reported an error is incomplete -- a fatal `file not found` leaves
    // every header behind that include out of it -- and a parse that loads it then reads a
    // program that is neither the source nor anything else. OpenCV's core parsed with 39
    // errors per unit on exactly that, and the split was decided on the result. Better no
    // PCH than that one.
    {
        const unsigned n = clang_getNumDiagnostics(tu);
        unsigned errors = 0;
        for (unsigned i = 0; i < n; ++i) {
            CXDiagnostic d = clang_getDiagnostic(tu, i);
            if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
                if (verbose && errors < 3)
                    out << "  libclang PCH diagnostic: "
                        << cx_to_string(clang_formatDiagnostic(d, CXDiagnostic_DisplaySourceLocation)) << "\n";
                ++errors;
            }
            clang_disposeDiagnostic(d);
        }
        if (errors) {
            if (verbose)
                out << "  libclang PCH: " << errors << " error(s) while building it; not"
                       " saved, parsing without it\n";
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return "";
        }
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
    // The unit's preamble cut after each inclusion of a pair header: what that inclusion's
    // pieces include first, and so what needs a PCH beside the preamble's. TODO/44 (B).
    std::vector<std::string> context_preambles;
};

struct SplitHeaderInfo {
    std::string split_dir;
    std::string preamble_path;
    std::vector<std::string> compilable_files;
};

static std::unordered_map<std::string, SplitHeaderInfo> g_split_headers;
// Variant copies of pair headers with the definitions in place -> the original. TODO/44 (B).
static std::map<std::string, std::string> g_definition_variants;
// Headers copied as they are because they include a split header beside themselves ->
// the original. TODO/47.
static std::map<std::string, std::string> g_verbatim_mirrors;


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
static std::string g_unit_main_file;   // the unit itself, normalised; its directives are the preamble's

static std::vector<std::string> unit_include_dirs(const std::vector<std::string>& flags) {
    std::vector<std::string> dirs = include_dirs_from_flags(flags);
    // Last, not sorted in: it only stands in for a directive the record did not see.
    if (!g_unit_source_dir.empty() &&
        std::find(dirs.begin(), dirs.end(), g_unit_source_dir) == dirs.end())
        dirs.push_back(g_unit_source_dir);
    return dirs;
}

static std::string path_hash(const std::string& s) {
    size_t h = std::hash<std::string>{}(s);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016zx", h);
    return std::string(buf);
}

// How each file the translation unit read was first spelled: the file that included it and
// the name written in the directive, from the preprocessing record. Filled once per parse
// by collect_inclusion_record(); header_mirror_relpath() reads it.
struct IncludeSpelling {
    std::string includer;   // absolute path of the file holding the directive
    std::string spelled;    // the name as written
};
static std::map<std::string, IncludeSpelling> g_include_spellings;
static std::map<std::string, std::string> g_mirror_rel_cache;

static std::string header_mirror_relpath(const std::string& abs_header,
                                         const std::vector<std::string>& include_dirs);

// Where a header's rewritten copy lives, relative to <split_dir>/include, decided from how
// the unit spelled its inclusion, so that the same directive finds the copy through
// -I<split_dir>/include:
//
//  - `#include "lib/cstring.h"` resolved through an include directory: the copy is at
//    `lib/cstring.h`. p4c includes everything relative to its root and sits its sources
//    in `lib/`; matching the longest include directory put the copy at `cstring.h`, where
//    the directive never looked, and the original's definitions came back into every piece
//    (TODO/47);
//  - `#include "cstring.h"` from `lib/bitrange.h`, resolved beside its includer: the copy
//    goes beside the includer's copy, `lib/cstring.h`, so that the copied directive finds
//    it the same way;
//  - a file the unit spelled two ways gets one copy, at the first spelling.
//
// Without a record of the inclusion -- a file named only by the AST -- the include
// directory that contains it decides, longest first, the unit's own directory last, and a
// header under none falls back to a hash of its absolute path, which keeps the mapping
// unique rather than colliding on the basename.
static std::string mirror_relpath_from_spelling(const std::string& abs_header,
                                                const std::vector<std::string>& include_dirs,
                                                int depth) {
    auto it = g_include_spellings.find(abs_header);
    if (it == g_include_spellings.end() || depth > 32) return std::string();
    const fs::path spelled = it->second.spelled;
    if (spelled.empty() || spelled.is_absolute()) return std::string();
    const fs::path h = fs::path(abs_header).lexically_normal();
    std::error_code ec;
    auto resolves_from = [&](const fs::path& dir) {
        return (dir / spelled).lexically_normal() == h;
    };
    auto usable = [](const std::string& rel) {
        return !rel.empty() && rel.rfind("..", 0) != 0 && rel[0] != '/';
    };
    const fs::path includer_dir = fs::path(it->second.includer).parent_path();
    const bool from_unit = it->second.includer == g_unit_main_file;
    // Beside its includer, which is what a quoted include tries first: the includer's copy
    // decides where this one goes, so that the copied directive finds it the same way. The
    // unit itself is not copied; its directory is an include directory of the pieces.
    if (!from_unit && resolves_from(includer_dir)) {
        const std::string inc_rel =
            mirror_relpath_from_spelling(it->second.includer, include_dirs, depth + 1);
        if (!inc_rel.empty()) {
            const std::string rel =
                (fs::path(inc_rel).parent_path() / spelled).lexically_normal().string();
            if (usable(rel)) return rel;
        }
    }
    for (const auto& d : include_dirs)
        if (resolves_from(fs::path(d))) {
            const std::string rel = spelled.lexically_normal().string();
            if (usable(rel)) return rel;
        }
    return std::string();
}

static std::string header_mirror_relpath(const std::string& abs_header,
                                         const std::vector<std::string>& include_dirs) {
    auto cached = g_mirror_rel_cache.find(abs_header);
    if (cached != g_mirror_rel_cache.end()) return cached->second;
    const std::string from_spelling =
        mirror_relpath_from_spelling(abs_header, include_dirs, 0);
    if (!from_spelling.empty()) {
        g_mirror_rel_cache[abs_header] = from_spelling;
        return from_spelling;
    }
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

    // Classic guard: `#ifndef X` followed within a few lines by `#define X` -- and it has to
    // be the file's first conditional directive. A guard wraps the whole file; an
    // `#ifndef X / #define X` pair anywhere else guards a section. OpenCV's arithm.simd.hpp
    // ends with `#ifndef SIMD_GUARD / #define SIMD_GUARD`, is meant to be included twice
    // under two macro states, and was taken for a guarded header on that pair -- so its
    // copy got a `#pragma once` and the second inclusion vanished.
    std::istringstream iss(blanked);
    std::string line, guard;
    int since_ifndef = -1;
    bool any_conditional = false;
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
        if (directive == "ifndef" && !ident.empty() && !any_conditional) {
            guard = ident;
            since_ifndef = 0;
        } else if (directive == "define" && since_ifndef >= 0 && ident == guard) {
            return true;
        } else if (since_ifndef >= 0 && ++since_ifndef > 4) {
            since_ifndef = -1;
        }
        if (directive == "if" || directive == "ifdef" || directive == "ifndef" ||
            directive == "elif" || directive == "else" || directive == "endif")
            any_conditional = true;
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

// ---------------------------------------------------------------------------------------
// TODO/28: skipping the parse when only a function body changed.
//
// split_inputs_hash() is all-or-nothing: it hashes the source, the flags and the contents of
// every prerequisite, and any difference sends the unit back through a full libclang parse.
// A one-line edit inside one body changes content, so it misses -- even though such an edit
// cannot change which definitions exist, their order, their linkage, or where any of them
// belongs.
//
// What it does change was measured rather than assumed. Adding a line inside
// side_info::collinear() and re-splitting a Boost.Geometry unit changes exactly two things:
// the piece for that definition, and the line numbers of the <tag>.keeps entries that follow
// it. The rewritten header copy, the unit preamble and every other piece come out
// byte-identical, because generate_preamble() replaces a moved definition with a declaration
// rebuilt from its FunctionInfo rather than from its text.
//
// So the fast path does not need an AST. It needs the previous harvest -- where each
// definition started and ended -- plus enough hashes to *prove* that everything outside one
// body is unchanged. That proof is the point: the offsets are a hypothesis, and the hashes
// are what turn it into a fact before anything is written.
//
// Why not reload a serialized AST instead, or reparse: TODO/29. Short version, both measured:
// clang_createTranslationUnit2() opens in 0.02s but deserializes lazily, so walking it costs
// 1.21s against 1.95s to parse and walk -- 1.6x, not worth the format; and
// clang_reparseTranslationUnit() refuses a loaded unit by construction and would re-parse
// anyway, its only shortcut being a preamble that a header edit invalidates.

// Version 5 carries definitions no piece was emitted for, written with "-" in the piece
// field. Bumped rather than made tolerant, so a harvest left by an older binary is refused
// outright -- it records those definitions inside a gap, and reading it as though it did not
// would put an edit to a kept body in the wrong place.
static const char* kHarvestMagic = "cpp-splitter-harvest 5";

struct HarvestDef {
    unsigned start = 0, end = 0;      // byte extent of the definition in its file
    unsigned body_open = 0;           // offset of the `{` that opens the body
    unsigned start_line = 0, end_line = 0;
    std::string piece;                // absolute path of the .cpp emitted for it, if any
    std::string extent_hash;          // hash of [start,end) as it was
    // Where the body sits inside the piece, and what it hashed to. The piece holds a
    // *transformed* extent -- `inline` may have been inserted, `static` stripped, an
    // always-inline attribute removed -- but every one of those edits lands in the prefix,
    // ahead of the opening brace, so the body itself is carried verbatim. Recording the
    // offset here is what lets a later run splice a new body in without holding a copy of
    // the old source, and the hash is what proves the offset still means what it did.
    size_t piece_body_off = 0;
    std::string body_hash;
    std::string prefix_hash;          // hash of [start, body_open): the signature
};

// One source file's contribution to a split: the definitions taken out of it, and hashes of
// everything between them. A file is described completely -- gaps.size() == defs.size()+1 --
// so "nothing outside this one body changed" is a check rather than a hope.
struct HarvestFile {
    std::string path;
    size_t size = 0;
    std::string undef_hash;           // hash of undefined_macros(), which gates placement
    std::vector<HarvestDef> defs;
    std::vector<std::string> gaps;
};

static std::string hash_bytes(const std::string& s) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016zx", std::hash<std::string>{}(s));
    return std::string(buf);
}

static std::string undefined_macros_hash(const std::string& source) {
    std::string joined;
    for (const auto& m : undefined_macros(source)) joined += m + "\n";
    return hash_bytes(joined);
}

// The `{` that opens a definition's body: the match of the final `}`. Scanning backwards is
// what makes this right for a constructor, whose member-initialiser list can hold braces of
// its own -- `Foo() : v{1, 2} { ... }` -- that a forward scan would mistake for the body.
// Returns npos when the extent does not end in a brace at all, which is reason enough to
// leave the definition out of the harvest rather than guess.
static size_t body_open_offset(const std::string& extent) {
    const std::string blanked = blank_code_noise(extent);
    size_t last = blanked.find_last_not_of(" \t\r\n");
    if (last == std::string::npos || blanked[last] != '}') return std::string::npos;
    int depth = 0;
    for (size_t i = last + 1; i-- > 0;) {
        if (blanked[i] == '}') ++depth;
        else if (blanked[i] == '{') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

static void write_harvest(const std::string& output_dir, const std::string& unit_tag,
                          const std::string& abs_path, const std::string& source,
                          const std::vector<HarvestDef>& defs_in) {
    const std::string path = (fs::path(output_dir) / (unit_tag + ".harvest")).string();

    // Definitions whose body is carried verbatim somewhere, in file order and
    // non-overlapping. A definition sharing its extent with another is left inside a gap,
    // where the gap hash still proves it did not change and an edit to it simply refuses.
    //
    // A *kept* definition is included, with no piece. This unit emitted nothing for it, so
    // its body lives in the unit's rewritten copy of the file, and that is what a later edit
    // patches. Excluding them left them inside a gap, which is why an edit to one read as a
    // change outside every definition and re-split the whole unit -- on Boost.Spirit, for the
    // 193 of 194 units that include the edited header without emitting the function.
    std::vector<HarvestDef> defs;
    for (const auto& d : defs_in) {
        if (d.body_hash.empty()) continue;
        if (d.end > source.size() || d.start >= d.end) continue;
        if (!defs.empty() && d.start < defs.back().end) continue;
        defs.push_back(d);
    }
    std::sort(defs.begin(), defs.end(),
              [](const HarvestDef& a, const HarvestDef& b) { return a.start < b.start; });

    std::ostringstream os;
    os << kHarvestMagic << "\n";
    os << abs_path << "\n";
    os << source.size() << "\n";
    os << undefined_macros_hash(source) << "\n";
    os << defs.size() << "\n";
    unsigned cursor = 0;
    for (const auto& d : defs) {
        os << "G " << hash_bytes(source.substr(cursor, d.start - cursor)) << "\n";
        os << "D " << d.start << " " << d.end << " " << d.body_open << " "
           << d.start_line << " " << d.end_line << " "
           << hash_bytes(source.substr(d.start, d.end - d.start)) << " "
           << d.piece_body_off << " " << d.body_hash << " " << d.prefix_hash << " "
           << (d.piece.empty() ? std::string("-") : d.piece) << "\n";
        cursor = d.end;
    }
    os << "G " << hash_bytes(source.substr(cursor)) << "\n";

    const std::string text = os.str();
    if (!fs::exists(path) || read_file(path) != text) {
        std::ofstream ofs(path);
        if (ofs.is_open()) ofs << text;
    }
}

static bool read_harvest(const fs::path& path, HarvestFile& hf) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;
    std::string line;
    if (!std::getline(ifs, line) || line != kHarvestMagic) return false;
    if (!std::getline(ifs, hf.path)) return false;
    if (!std::getline(ifs, line)) return false;
    try { hf.size = std::stoull(line); } catch (...) { return false; }
    if (!std::getline(ifs, hf.undef_hash)) return false;
    size_t ndefs = 0;
    if (!std::getline(ifs, line)) return false;
    try { ndefs = std::stoull(line); } catch (...) { return false; }

    for (size_t i = 0; i < ndefs; ++i) {
        if (!std::getline(ifs, line) || line.compare(0, 2, "G ") != 0) return false;
        hf.gaps.push_back(line.substr(2));
        if (!std::getline(ifs, line) || line.compare(0, 2, "D ") != 0) return false;
        std::istringstream ds(line.substr(2));
        HarvestDef d;
        if (!(ds >> d.start >> d.end >> d.body_open >> d.start_line >> d.end_line
                 >> d.extent_hash >> d.piece_body_off >> d.body_hash >> d.prefix_hash))
            return false;
        std::getline(ds >> std::ws, d.piece);
        if (d.piece.empty()) return false;      // the field is never absent
        if (d.piece == "-") d.piece.clear();    // kept: no piece was emitted for it
        // "=" stays: declared only in the copy (TODO/48); nothing carries the body.
        hf.defs.push_back(d);
    }
    if (!std::getline(ifs, line) || line.compare(0, 2, "G ") != 0) return false;
    hf.gaps.push_back(line.substr(2));
    return hf.gaps.size() == hf.defs.size() + 1;
}

// Which prerequisite contents were seen by the run that wrote this split.
static void write_inputs_hashes(const std::string& split_dir, const std::string& input_file) {
    const std::string dep_cache = (fs::path(split_dir) / "depfile.cache").string();
    if (!fs::exists(dep_cache)) return;
    std::ostringstream os;
    // Each prerequisite once. The depfile already names the source -- rewrite_depfile()
    // puts it first -- and when the build spelled it the same way (CMake passes absolute
    // paths) it was recorded twice, so a body edit read as two changed inputs and the
    // re-slice never ran under CMake. TODO/43.
    std::set<std::string> seen;
    auto record = [&](const std::string& path) {
        std::error_code ec;
        const std::string key = fs::absolute(path, ec).lexically_normal().string();
        if (!seen.insert(ec ? path : key).second) return;
        const std::string h = file_content_hash(path);
        os << path << "\t" << (h.empty() ? "<absent>" : h) << "\n";
    };
    record(input_file);
    const std::string deps = read_file(dep_cache);
    const size_t colon = deps.find(':');
    if (colon == std::string::npos) return;
    std::istringstream tokens(deps.substr(colon + 1));
    std::string token;
    while (tokens >> token) {
        if (token == "\\") continue;
        record(token);
    }
    std::ofstream ofs((fs::path(split_dir) / "inputs.hash").string());
    if (ofs.is_open()) ofs << os.str();
}

static std::vector<std::string> changed_prerequisites(const std::string& split_dir) {
    std::vector<std::string> changed;
    std::ifstream ifs((fs::path(split_dir) / "inputs.hash").string());
    if (!ifs.is_open()) return {"<no record>"};
    std::string line;
    while (std::getline(ifs, line)) {
        const size_t tab = line.rfind('\t');
        if (tab == std::string::npos) return {"<corrupt>"};
        const std::string path = line.substr(0, tab), was = line.substr(tab + 1);
        const std::string now = file_content_hash(path);
        if ((now.empty() ? "<absent>" : now) != was) changed.push_back(path);
    }
    return changed;
}

// Replace `old_text` in `path` with `new_text`, refusing unless it occurs exactly once.
static bool patch_file_once(const std::string& path, const std::string& old_text,
                            const std::string& new_text) {
    const std::string body = read_file(path);
    const size_t at = body.find(old_text);
    if (at == std::string::npos) return false;
    if (body.find(old_text, at + 1) != std::string::npos) return false;
    std::string updated = body.substr(0, at) + new_text + body.substr(at + old_text.size());
    std::ofstream ofs(path);
    if (!ofs.is_open()) return false;
    ofs << updated;
    return true;
}

static unsigned count_newlines(const std::string& s) {
    return (unsigned)std::count(s.begin(), s.end(), '\n');
}

// How many conditionals a definition's own text closes that it did not open: `#endif`s in
// the extent beyond those matching an `#if` inside it. A declarator written across
// `#ifdef ... #else ... #endif` with the body after the `#endif` -- OpenCV's alloc.cpp
// `fastMalloc` -- has an extent that begins inside the `#else` branch and contains the
// `#endif` closing it. The conditional stack replayed around the piece opens that branch, so
// the piece must not close it a second time.
static size_t conditionals_closed_by(const std::string& text) {
    const std::string blanked = blank_code_noise(text);
    long depth = 0;
    size_t closed = 0;
    std::istringstream iss(blanked);
    std::string line;
    while (std::getline(iss, line)) {
        const size_t at = line.find_first_not_of(" \t");
        if (at == std::string::npos || line[at] != '#') continue;
        std::istringstream ls(line.substr(at + 1));
        std::string directive;
        ls >> directive;
        if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
            ++depth;
        } else if (directive == "endif") {
            if (depth > 0) --depth; else ++closed;
        }
    }
    return closed;
}

static void emit_split_files(CXTranslationUnit tu,
                             const std::vector<FunctionInfo>& functions,
                             const std::vector<VariableInfo>& variables,
                             const std::string& input_path,
                             const std::string& abs_path,
                             const std::string& output_dir,
                             const std::string& include_root,
                             const std::string& unit_tag,
                             unsigned inclusion,
                             const std::string& preamble_filename,
                             const std::string& preamble_path,
                             const std::string& definitions_filename,
                             const std::string& definitions_context,
                             const std::string& definitions_variant,
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

// Every file the translation unit read that may hold a definition.
//
// clang_getInclusions() is not the whole list. With a precompiled preamble in play -- and one
// is, see the parse flags in do_split() -- libclang 13 walks either the loaded source-location
// table (the preamble's inclusions) or the local one, not both, so an include placed after the
// unit's opening include block is never reported. OpenCV's filesystem.cpp ends with
// `#include "plugin_loader.impl.hpp"`, which defines DynamicLib::~DynamicLib() out of line;
// unreported, the header was never split, the destructor stayed in the preamble verbatim, and
// every piece of the unit emitted it. Even when it was split, the same call decided whose
// pieces were compiled and linked, so the split was written and then left out of the object.
//
// The list is therefore completed from the AST: every file a function or variable definition
// sits in is appended, whatever reported its inclusion. Both callers use this one function so
// that a header is never split by one and forgotten by the other.
// Set by header_split_candidates() when a header it will not split defines a function with
// external linkage and no `inline`. Such a definition reaches every piece through the
// preamble, which includes the header as it is, and the pieces' objects then define it as
// many times as there are pieces. Nothing downstream can repair that, so do_split() stops
// before writing anything and the unit is compiled whole -- the same outcome the relocatable
// link would have forced, without the pieces being compiled first.
static std::string g_unsplittable_header;
static std::string g_unsplittable_header_reason;

// Files in which the translation unit defines a function with external linkage and no
// `inline`: the ones whose text cannot be shared by every piece.
static std::set<std::string> files_with_external_definitions(CXTranslationUnit tu) {
    std::set<std::string> files;
    auto visit = [](CXCursor c, CXCursor, CXClientData d) -> CXChildVisitResult {
        const CXCursorKind k = clang_getCursorKind(c);
        const bool fn_kind =
            k == CXCursor_FunctionDecl || k == CXCursor_CXXMethod ||
            k == CXCursor_Constructor || k == CXCursor_Destructor ||
            k == CXCursor_ConversionFunction;
        if (fn_kind && clang_isCursorDefinition(c)) {
            if (!clang_Cursor_isFunctionInlined(c) &&
                clang_getCursorLinkage(c) == CXLinkage_External) {
                CXFile f = nullptr;
                clang_getFileLocation(clang_getCursorLocation(c), &f, nullptr, nullptr, nullptr);
                if (f) {
                    std::error_code ec;
                    const std::string abs =
                        fs::absolute(cx_to_string(clang_getFileName(f)), ec).lexically_normal().string();
                    if (!ec) static_cast<std::set<std::string>*>(d)->insert(abs);
                }
            }
            return CXChildVisit_Continue;
        }
        return CXChildVisit_Recurse;
    };
    clang_visitChildren(clang_getTranslationUnitCursor(tu), visit, &files);
    return files;
}

static std::vector<std::string> inclusions_of(CXTranslationUnit tu) {
    std::vector<std::string> includes;

    // The preprocessing record first: every directive the preprocessor acted on, in order,
    // once per inclusion. A header included twice under different macro states -- OpenCV's
    // arithm.simd.hpp, definitions then dispatchers -- has to be counted twice, or the rule
    // that leaves such a pair alone never fires and its copy gets a `#pragma once` that
    // drops the second inclusion.
    auto directives = [](CXCursor c, CXCursor, CXClientData d) -> CXChildVisitResult {
        if (clang_getCursorKind(c) == CXCursor_InclusionDirective) {
            CXFile f = clang_getIncludedFile(c);
            if (f) {
                std::error_code ec;
                const std::string abs =
                    fs::absolute(cx_to_string(clang_getFileName(f)), ec).lexically_normal().string();
                if (!ec) static_cast<std::vector<std::string>*>(d)->push_back(abs);
            }
        }
        return CXChildVisit_Continue;
    };
    clang_visitChildren(clang_getTranslationUnitCursor(tu), directives, &includes);

    // Without a record -- a translation unit parsed without the flag -- the older source.
    if (includes.empty()) clang_getInclusions(tu, inclusion_visitor, &includes);

    // And whatever holds a definition, whether or not a directive was seen for it.
    std::set<std::string> from_ast;
    auto visit = [](CXCursor c, CXCursor, CXClientData d) -> CXChildVisitResult {
        auto* files = static_cast<std::set<std::string>*>(d);
        const CXCursorKind k = clang_getCursorKind(c);
        const bool def_kind =
            k == CXCursor_FunctionDecl || k == CXCursor_CXXMethod ||
            k == CXCursor_Constructor || k == CXCursor_Destructor ||
            k == CXCursor_ConversionFunction || k == CXCursor_FunctionTemplate ||
            k == CXCursor_VarDecl;
        if (def_kind && clang_isCursorDefinition(c)) {
            CXFile f = nullptr;
            clang_getFileLocation(clang_getCursorLocation(c), &f, nullptr, nullptr, nullptr);
            if (f) {
                std::error_code ec;
                const std::string abs =
                    fs::absolute(cx_to_string(clang_getFileName(f)), ec).lexically_normal().string();
                if (!ec) files->insert(abs);
            }
            return CXChildVisit_Continue;
        }
        return CXChildVisit_Recurse;
    };
    clang_visitChildren(clang_getTranslationUnitCursor(tu), visit, &from_ast);
    std::set<std::string> reported(includes.begin(), includes.end());
    // A module unit's definitions are in the AST through an import, not an inclusion; it is
    // not a header and must not be mirrored as one. TODO/43.
    for (const auto& f : from_ast)
        if (!reported.count(f) && !is_module_unit_file(f)) includes.push_back(f);
    return includes;
}

// The #include lines of the unit itself -- written in the source, or in the prefix the PCH
// was built from, whose lines are the source's -- by the file each resolved to, in the order
// the preprocessor took them. TODO/44 (B).
static void collect_inclusion_sites(CXTranslationUnit tu,
                                    const std::string& main_file,
                                    const std::string& prefix_file) {
    g_inclusion_sites.clear();
    g_include_spellings.clear();
    g_mirror_rel_cache.clear();
    g_verbatim_mirrors.clear();
    struct Ctx { const std::string* main; const std::string* prefix; };
    Ctx ctx{&main_file, &prefix_file};
    auto directives = [](CXCursor c, CXCursor, CXClientData d) -> CXChildVisitResult {
        if (clang_getCursorKind(c) != CXCursor_InclusionDirective) return CXChildVisit_Continue;
        const Ctx* ctx = static_cast<const Ctx*>(d);
        CXFile included = clang_getIncludedFile(c);
        CXFile in = nullptr;
        unsigned line = 0;
        clang_getFileLocation(clang_getCursorLocation(c), &in, &line, nullptr, nullptr);
        if (!included || !in) return CXChildVisit_Continue;
        std::error_code ec;
        const std::string in_path =
            fs::absolute(cx_to_string(clang_getFileName(in)), ec).lexically_normal().string();
        if (ec) return CXChildVisit_Continue;
        const std::string target =
            fs::absolute(cx_to_string(clang_getFileName(included)), ec).lexically_normal().string();
        if (ec) return CXChildVisit_Continue;
        const std::string spelled = cx_to_string(clang_getCursorSpelling(c));
        // The first spelling of every file, from whichever file wrote it; a directive in
        // the prefix belongs to the unit, whose lines the prefix's are.
        if (!g_include_spellings.count(target))
            g_include_spellings[target] =
                IncludeSpelling{in_path == *ctx->prefix ? *ctx->main : in_path, spelled};
        if (in_path == *ctx->main || in_path == *ctx->prefix)
            g_inclusion_sites[target].emplace_back(line, spelled);
        return CXChildVisit_Continue;
    };
    clang_visitChildren(clang_getTranslationUnitCursor(tu), directives, &ctx);
    if (std::getenv("CPP_SPLITTER_DUMP_INCLUDES"))
        for (const auto& e : g_include_spellings)
            std::cerr << "[include] " << e.first << " <- " << e.second.includer << " as \""
                      << e.second.spelled << "\"\n";
}

// Once the translation unit is harvested: re-key what each pair header defines by inclusion
// (`path` for the first, `path#n` after), or take the header out of the candidate list when
// its inclusions cannot be told apart. Fills g_preamble_include_rewrites.
static void attribute_pair_inclusions(HarvestMap& harvest,
                                      VarHarvestMap& var_harvest,
                                      std::vector<std::string>& candidates,
                                      const std::set<std::string>& external_definers,
                                      const std::string& main_file,
                                      const std::string& source,
                                      const std::vector<std::string>& inc_dirs,
                                      const std::string& output_dir,
                                      bool verbose,
                                      std::ostream& out) {
    for (const auto& path : g_pair_headers) {
        const auto sites = g_inclusion_sites.find(path);
        const size_t count = sites == g_inclusion_sites.end() ? 0 : sites->second.size();
        const std::vector<unsigned> bases = ordered_inclusion_bases(path);
        // Every inclusion reused from an earlier run: nothing was harvested and there is
        // nothing to attribute, but the preamble still has to name the later copies.
        const bool harvested =
            std::any_of(candidates.begin(), candidates.end(), [&](const std::string& c) {
                return decode_inclusion_key(c).first == path;
            });
        if (harvested && (bases.size() != count || count < 2)) {
            if (verbose)
                out << "Skipping " << path << ": read " << count << " times but "
                    << bases.size() << " inclusion(s) hold declarations, so its inclusions"
                       " cannot be told apart\n";
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                            [&](const std::string& c) {
                                                return decode_inclusion_key(c).first == path;
                                            }),
                             candidates.end());
            harvest.erase(path);
            var_harvest.erase(path);
            if (external_definers.count(path) && g_unsplittable_header.empty()) {
                g_unsplittable_header = path;
                g_unsplittable_header_reason = "its inclusions cannot be told apart";
            }
            continue;
        }
        auto fit = harvested ? harvest.find(path) : harvest.end();
        if (fit != harvest.end()) {
            std::vector<FunctionInfo> first;
            for (auto& fn : fit->second) {
                const unsigned n = inclusion_of_raw(bases, fn.sloc_raw);
                if (n <= 1) first.push_back(std::move(fn));
                else harvest[inclusion_key(path, n)].push_back(std::move(fn));
            }
            fit->second.swap(first);
        }
        auto vit = harvested ? var_harvest.find(path) : var_harvest.end();
        if (vit != var_harvest.end()) {
            std::vector<VariableInfo> first;
            for (auto& var : vit->second) {
                const unsigned n = inclusion_of_raw(bases, var.sloc_raw);
                if (n <= 1) first.push_back(std::move(var));
                else var_harvest[inclusion_key(path, n)].push_back(std::move(var));
            }
            vit->second.swap(first);
        }
        const std::string rel = header_mirror_relpath(path, inc_dirs);
        for (size_t n = 2; n <= count; ++n) {
            // Only an inclusion that gets a copy is named: one that defines nothing is not
            // split, and the preamble keeps reading the original. OpenCV's
            // ccl_bolelli_forest_singleline.inc.hpp is a fragment of a function body,
            // included in two functions.
            const std::string key = inclusion_key(path, static_cast<unsigned>(n));
            const bool has_copy =
                harvested ? (harvest.count(key) && !harvest[key].empty()) ||
                                (var_harvest.count(key) && !var_harvest[key].empty())
                          : fs::exists((fs::path(split_include_root(output_dir)) /
                                        inclusion_mirror_relpath(rel, static_cast<unsigned>(n)))
                                           .string());
            if (!has_copy) continue;
            const auto& site = sites->second[n - 1];
            PreambleIncludeRewrite rw;
            rw.unit = main_file;
            rw.spelled = site.second;
            rw.occurrence = include_occurrence_before(source, site.first, rw.spelled);
            rw.replacement = inclusion_mirror_relpath(rel, static_cast<unsigned>(n));
            g_preamble_include_rewrites.push_back(rw);
            if (verbose)
                out << "[auto-split] inclusion " << n << " of " << path << " (line "
                    << site.first << ") -> " << rw.replacement << "\n";
        }
    }
}

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

    const std::vector<std::string> includes = inclusions_of(tu);
    const std::set<std::string> external_definers = files_with_external_definitions(tu);

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
        // Every reason a header is not split is said, or a header that silently kept its
        // definitions in the preamble -- and had every piece emit them -- cannot be told from
        // one that was never included.
        if (is_stdlib_header(inc_path)) {
            if (verbose) out << "[auto-split] not split, system header: " << inc_path << "\n";
            continue;
        }
        const std::string rel = header_mirror_relpath(inc_path, inc_dirs);
        const std::string manifest = (fs::path(include_root) / (rel + ".split")).string();

        // A file with no include guard read more than once is one file and several
        // inclusions, each under its own macro state, and one copy cannot serve them all.
        // Each inclusion written in the unit itself is split on its own (TODO/44 B); a pair
        // one of whose inclusions comes through another header is left as it is, and if it
        // defines a function that may exist in only one object the unit cannot be split at
        // all, since the definition reaches every piece through the preamble. Read exactly
        // once, the file is an implementation include, which has no guard for the ordinary
        // reason that it does not need one.
        if (inclusion_count[inc_path] > 1 && !header_has_include_guard(read_file(inc_path))) {
            const auto sites = g_inclusion_sites.find(inc_path);
            const size_t in_unit = sites == g_inclusion_sites.end() ? 0 : sites->second.size();
            if (in_unit != static_cast<size_t>(inclusion_count[inc_path])) {
                if (verbose)
                    out << "Skipping " << inc_path << ": no include guard and included "
                        << inclusion_count[inc_path] << " times, " << in_unit
                        << " of them from the unit itself\n";
                if (external_definers.count(inc_path) && g_unsplittable_header.empty()) {
                    g_unsplittable_header = inc_path;
                    g_unsplittable_header_reason =
                        "not every inclusion is written in the unit";
                }
                const std::string unit_dir =
                    (fs::path(include_root) / fs::path(rel).parent_path()).string();
                write_skipped_header_manifest(unit_dir, fs::path(inc_path).filename().string(),
                                              inc_path, "no include guard, included repeatedly");
                continue;
            }
            g_pair_headers.insert(inc_path);
            for (unsigned n = 1; n <= static_cast<unsigned>(in_unit); ++n) {
                const std::string key = inclusion_key(inc_path, n);
                if (g_split_headers.find(key) != g_split_headers.end()) {
                    if (verbose) out << "[auto-split] already split: " << key << "\n";
                    continue;
                }
                const std::string manifest_n =
                    (fs::path(include_root) / (inclusion_mirror_relpath(rel, n) + ".split")).string();
                std::error_code ec;
                if (fs::exists(manifest_n) &&
                    fs::last_write_time(inc_path, ec) <= fs::last_write_time(manifest_n, ec) &&
                    !ec &&
                    read_file(manifest_n).find("# not split: no include guard") ==
                        std::string::npos) {
                    register_header_manifest(manifest_n, include_root);
                    if (verbose)
                        out << "[auto-split] reusing an earlier split of " << key << "\n";
                    continue;
                }
                candidates.push_back(key);
            }
            continue;
        }

        if (g_split_headers.find(inc_path) != g_split_headers.end()) {
            if (verbose) out << "[auto-split] already split: " << inc_path << "\n";
            continue;
        }

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
            if (verbose) out << "[auto-split] reusing an earlier split of " << inc_path << "\n";
            continue;
        }

        if (read_file(inc_path).empty()) continue;
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
                              std::ostream& out,
                              // Which inclusion of a header read more than once this is;
                              // the second and later ones are mirrored under
                              // include/_inclusion/<n>/. TODO/44 (B).
                              unsigned inclusion = 1,
                              // For such an inclusion, the unit's preamble cut *before* its
                              // #include line: what the definitions piece includes ahead of
                              // a variant of the copy that keeps the definitions in place.
                              const std::string& definitions_context = std::string()) {
    SplitResult result;
    result.success = false;

    // A module interface unit's "preamble" is the interface the compiler precompiles: the
    // unit with its non-inline bodies replaced by declarations, under the unit's own
    // extension so the compiler still takes it as a module unit. TODO/43.
    const bool module_interface = !input_is_header && g_module_unit.interface;
    const std::string preamble_filename = input_is_header
        ? fs::path(abs_path).filename().string()
        : module_interface
            ? fs::path(abs_path).stem().string() + "_interface" + fs::path(abs_path).extension().string()
            : fs::path(abs_path).stem().string() + "_preamble.h";

    std::string unit_dir = output_dir;
    if (input_is_header) {
        const std::string rel = inclusion_mirror_relpath(
            header_mirror_relpath(abs_path, unit_include_dirs(extra_flags)), inclusion);
        unit_dir = (fs::path(split_include_root(output_dir)) /
                    fs::path(rel).parent_path()).string();
    }

    const std::string unit_tag = sanitize_filename(fs::path(abs_path).filename().string());
    const std::string preamble_path = (fs::path(unit_dir) / preamble_filename).string();

    prepare_functions(functions, source, referenced, input_is_header);
    g_unmovable_static_variable.clear();
    prepare_variables(variables, functions, source, unit_tag, input_is_header);
    dump_keep_decisions(functions);

    // A function with internal linkage kept in the preamble is one per piece; with a local
    // `static` in it, so is its state. Decline rather than duplicate it. TODO/47.
    if (!input_is_header)
        for (const auto& fn : functions)
            if (fn.keep_in_header && fn.has_local_static && !fn.external_linkage &&
                !fn.is_inlined && !fn.is_template && !fn.in_class_template) {
                g_declined = true;
                std::cerr << "[cpp-splitter] not splitting " << input_path << ": '"
                          << fn.qualified_name << "' has internal linkage, is kept in the"
                             " preamble (" << keep_reason(fn) << ") and holds a local static;"
                             " every piece would get its own\n";
                result.success = false;
                return result;
            }

    // A `static` variable no rule could move: left in the preamble it would be one object
    // per piece and the link would not say so. Decline instead. TODO/47.
    if (!g_unmovable_static_variable.empty()) {
        g_declined = true;
        std::cerr << "[cpp-splitter] not splitting " << input_path << ": the static variable '"
                  << g_unmovable_static_variable
                  << "' has a shape no rule can move out of the preamble, and left there it"
                     " would be one object per piece\n";
        result.success = false;
        return result;
    }

    // A pair header's conditionals test the macros the unit defines before each #include
    // and undefines after it; replayed at the end of the preamble they are all false and the
    // piece is empty. The harvest of inclusion n holds what was live under n's macro state,
    // so every conditional a piece of it replays is true. TODO/44 (B).
    if (input_is_header && g_pair_headers.count(abs_path))
        for (auto& fn : functions)
            for (auto& c : fn.conditionals) c = "#if 1";

    // A static variable whose type no other translation unit can name moves whole to the
    // definitions header (prepare_variables()), and every function that references it goes
    // there too -- that is the one translation unit that may hold single instances, and the
    // reference is what a template or a header-defined function could not carry along. Those
    // decline the unit, with the reason, as the variable alone did before TODO/44.
    if (!input_is_header) {
        for (const auto& var : variables) {
            if (!var.anchors_users || var.usr.empty()) continue;
            for (auto& fn : functions) {
                auto it = g_references.find(fn.usr);
                if (it == g_references.end() || !it->second.count(var.usr)) continue;
                if (fn.is_template || fn.in_class_template || is_included_file(fn.file)) {
                    g_declined = true;
                    std::cerr << "[cpp-splitter] not splitting " << input_path
                              << ": the static variable `" << var.name
                              << "` has a type no other translation unit can name, and `"
                              << fn.signature << "` refers to it from a "
                              << (is_included_file(fn.file) ? "header" : "template")
                              << ", which every piece would carry\n";
                    result.success = false;
                    return result;
                }
                fn.keep_in_header = true;
                fn.kept_with_variable = true;
            }
        }
    }

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
    g_definitions_need_variant = false;
    // A module interface has no definitions header: what may exist in only one object -- a
    // defaulted member written out of line, a non-inline variable -- stays where it is, in
    // the interface, which is compiled exactly once. With no sink, generate_preamble()
    // leaves such a definition in place.
    std::string preamble =
        generate_preamble(source, functions, variables, unit_tag, abs_path, static_renames,
                          module_interface ? nullptr : &definitions);
    if (module_interface) {
        // Not a header: no include guard, and `#pragma once` in a main file only warns.
        if (preamble.rfind("#pragma once\n", 0) == 0) preamble.erase(0, 13);
        // The global module fragment, replayed by every piece ahead of `module M;`: an
        // implementation unit imports its interface implicitly, but not the interface's
        // global module fragment.
        const std::string gmf_path =
            (fs::path(unit_dir) / (fs::path(abs_path).stem().string() + "_preamble.h")).string();
        const std::string gmf = "#pragma once\n" + g_module_unit.gmf;
        if (!fs::exists(gmf_path) || read_file(gmf_path) != gmf) {
            std::ofstream ofs(gmf_path);
            if (ofs.is_open()) ofs << gmf;
            if (verbose) out << "Generated global module fragment: " << gmf_path << "\n";
        }
    }

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

    // A pair header's definitions cannot be re-expanded behind the whole header: OpenCV's
    // arithm.simd.hpp redefines DEFINE_SIMD_FUN section by section, and DEFINE_SIMD_ALL(add,
    // op_add) moved to the definitions header expanded with the last section's macros. The
    // definitions piece of such an inclusion instead includes the preamble cut before the
    // inclusion and then a variant of the copy in which the pieces' functions are declared
    // and everything bound for the definitions header is left where it was written, so each
    // definition is expanded once, in the macro state it had. TODO/44 (B).
    std::string definitions_variant;
    if (!definitions_filename.empty() &&
        (!definitions_context.empty() || g_definitions_need_variant)) {
        definitions_variant = preamble_filename + ".definitions.h";
        const std::string variant_path = (fs::path(unit_dir) / definitions_variant).string();
        const std::string body = generate_preamble(source, functions, variables, unit_tag,
                                                   abs_path, static_renames, nullptr);
        if (!fs::exists(variant_path) || read_file(variant_path) != body) {
            std::ofstream ofs(variant_path);
            if (!ofs.is_open()) {
                std::cerr << "Error: cannot write " << variant_path << "\n";
                return result;
            }
            ofs << body;
            if (verbose) out << "Generated definitions variant: " << variant_path << "\n";
        }
        g_definition_variants[variant_path] = abs_path;
    }

    if (nothing_of_its_own) {
        result.success = true;
        return result;
    }

    emit_split_files(tu, functions, variables, input_path, abs_path, unit_dir,
                     split_include_root(output_dir), unit_tag, inclusion,
                     preamble_filename, preamble_path, definitions_filename,
                     definitions_context, definitions_variant,
                     all_flags, extra_flags,
                     context_preamble, input_is_header, parse_clean, verbose, out, result);
    result.success = true;
    return result;
}

// Splits every header this translation unit should split, using the inventory harvested
// from `tu` rather than re-parsing each header on its own, then collects the objects and
// include directories the caller has to link and compile against.

// Make a rewritten header's quoted includes resolve where they did before it was moved.
// TODO/31.
//
// `#include "x.hpp"` is looked up relative to the directory of the file doing the including.
// A rewritten copy lives in the mirror under <split_dir>/include, so that directory is now the
// one searched -- and the mirror holds a copy only for headers that were themselves split.
// Boost.Spirit's lexer headers include their siblings this way:
//
//     generator.hpp:10:10: fatal error: 'char_traits.hpp' file not found
//
// and no -I can rescue it, because the name is unqualified while the original sits several
// directories down an include root.
//
// This runs as a pass over the copies once they have all been written, rather than while each
// one is generated, and that is deliberate: whether a sibling ends up in the mirror is not
// known in advance. A header on the candidate list can still produce no copy -- it may have no
// definitions of its own, or fail to parse standalone -- so asking the filesystem afterwards
// is the only answer that is actually true.
// A header that is not split but includes a split header beside itself -- p4c's
// ir-generator.h, `#include "irclass.h"` -- is read as the original, and the quoted include
// then finds the original irclass.h next to it before any -I reaches the copy: every piece
// got the definitions back. Such a header is mirrored as it is, at its own mirror path, so
// that its copied directive finds the copy beside it; fix_mirror_quoted_includes() then
// points its other quoted includes at their originals. The walk goes up through includers
// until the unit itself, which the preamble stands in for. TODO/47.
static void mirror_includers_of_split_headers(const std::string& output_dir,
                                              const std::vector<std::string>& inc_dirs,
                                              const std::string& main_file,
                                              bool verbose, std::ostream& out) {
    const std::string mirror_root = split_include_root(output_dir);
    std::set<std::string> mirrored;
    for (const auto& entry : g_split_headers) mirrored.insert(decode_inclusion_key(entry.first).first);
    std::vector<std::string> work(mirrored.begin(), mirrored.end());
    while (!work.empty()) {
        const std::string header = work.back();
        work.pop_back();
        auto sp = g_include_spellings.find(header);
        if (sp == g_include_spellings.end()) continue;
        const std::string& includer = sp->second.includer;
        if (includer == main_file || mirrored.count(includer) || is_stdlib_header(includer)) continue;
        const fs::path includer_dir = fs::path(includer).parent_path();
        if ((includer_dir / sp->second.spelled).lexically_normal().string() !=
            fs::path(header).lexically_normal().string())
            continue;   // resolved through -I: the copy is found the same way
        const std::string rel = header_mirror_relpath(includer, inc_dirs);
        if (rel.empty()) continue;
        const std::string copy = (fs::path(mirror_root) / rel).string();
        const std::string text = read_file(includer);
        if (text.empty()) continue;
        std::error_code ec;
        fs::create_directories(fs::path(copy).parent_path(), ec);
        if (!fs::exists(copy) || read_file(copy) != text) {
            std::ofstream ofs(copy);
            if (!ofs.is_open()) continue;
            ofs << text;
        }
        g_verbatim_mirrors[copy] = includer;
        mirrored.insert(includer);
        work.push_back(includer);
        if (verbose)
            out << "[auto-split] mirrored as it is: " << includer << " (includes "
                << sp->second.spelled << " beside itself)\n";
    }
}

static void fix_mirror_quoted_includes(const std::string& output_dir,
                                       const std::vector<std::string>& inc_dirs,
                                       bool verbose, std::ostream& out) {
    const std::string mirror_root = split_include_root(output_dir);

    // Every rewritten copy: one per split header (per inclusion, for a pair header), and the
    // definitions variants of pair headers. A copy under include/_inclusion/<n>/ is not
    // parallel to the original tree, so a mirrored sibling has to be named by its path too.
    // TODO/44 (B).
    struct Copy { std::string original, path; bool parallel_to_original; };
    std::vector<Copy> copies;
    for (const auto& entry : g_split_headers) {
        const auto decoded = decode_inclusion_key(entry.first);
        copies.push_back({decoded.first, entry.second.preamble_path, decoded.second <= 1});
    }
    for (const auto& entry : g_definition_variants) {
        const bool parallel =
            entry.first.find("/_inclusion/") == std::string::npos;
        copies.push_back({entry.second, entry.first, parallel});
    }
    for (const auto& entry : g_verbatim_mirrors)
        copies.push_back({entry.second, entry.first, true});
    for (const auto& copy : copies) {
        const std::string& original = copy.original;
        const bool parallel_to_original = copy.parallel_to_original;
        const std::string& copy_path = copy.path;
        if (copy_path.empty() || !fs::exists(copy_path)) continue;

        const fs::path orig_dir = fs::path(original).parent_path();
        const std::string text = read_file(copy_path);
        if (text.empty()) continue;

        std::string rebuilt;
        rebuilt.reserve(text.size());
        size_t pos = 0;
        int rewritten = 0;
        while (pos <= text.size()) {
            size_t eol = text.find('\n', pos);
            const bool last = (eol == std::string::npos);
            if (last) eol = text.size();
            std::string line = text.substr(pos, eol - pos);

            // Only a line whose first non-blank character is `#` can be a directive, which
            // keeps this away from the word "include" inside code or a comment.
            const size_t hash = line.find_first_not_of(" \t");
            if (hash != std::string::npos && line[hash] == '#') {
                const size_t kw = line.find("include", hash + 1);
                const size_t q1 = (kw == std::string::npos) ? std::string::npos
                                                           : line.find('"', kw + 7);
                const size_t q2 = (q1 == std::string::npos) ? std::string::npos
                                                           : line.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    const std::string target = line.substr(q1 + 1, q2 - q1 - 1);
                    const fs::path resolved =
                        (orig_dir / target).lexically_normal();
                    std::error_code ec;
                    if (!target.empty() && target[0] != '/' && fs::exists(resolved, ec)) {
                        // If the sibling is mirrored too, leave the directive alone. The
                        // mirror is structurally parallel to the original tree, so the same
                        // relative path still names the right file -- and pointing this one
                        // at the original instead would read a header whose definitions have
                        // been split out, defining every one of them a second time.
                        const std::string sib_rel =
                            header_mirror_relpath(resolved.string(), inc_dirs);
                        const std::string sib_copy = (fs::path(mirror_root) / sib_rel).string();
                        const bool sibling_mirrored =
                            !sib_rel.empty() && fs::exists(sib_copy, ec);
                        if (!sibling_mirrored) {
                            line = line.substr(0, q1 + 1) + resolved.string() +
                                   line.substr(q2);
                            ++rewritten;
                        } else if (!parallel_to_original) {
                            line = line.substr(0, q1 + 1) + sib_copy + line.substr(q2);
                            ++rewritten;
                        }
                    }
                }
            }

            rebuilt += line;
            if (!last) rebuilt += '\n';
            if (last) break;
            pos = eol + 1;
        }

        if (rewritten > 0 && rebuilt != text) {
            std::ofstream ofs(copy_path);
            if (ofs.is_open()) {
                ofs << rebuilt;
                if (verbose)
                    out << "  [quoted includes] " << rewritten << " rewritten in "
                        << copy_path << "\n";
            }
        }
    }
}

// The pieces of a pair header's inclusion n compile in the macro state of that inclusion:
// what the unit defined before the n-th #include and had not yet undefined after it. The
// whole preamble ends in the state of the last inclusion and whatever follows -- OpenCV's
// arithm.dispatch.cpp defines ARITHM_DISPATCHING_ONLY between its two inclusions of
// arithm.simd.hpp, so the SIMD kernels of the first, re-expanded behind the whole preamble,
// became dispatchers calling functions that do not exist. Each inclusion therefore gets the
// preamble cut right after its own #include line, with a PCH of its own. Written on every
// run that regenerates the preamble, since the pieces name these files; returns the file name
// for each inclusion's key. TODO/44 (B).
struct PairContext {
    std::string after;    // the preamble through the inclusion's #include line
    std::string before;   // the preamble up to that line: the definitions piece's context
};

static std::map<std::string, PairContext> write_pair_context_preambles(
        const std::string& preamble_path,
        const std::string& source,
        const std::vector<std::string>& inc_dirs,
        bool verbose,
        std::ostream& out) {
    std::map<std::string, PairContext> contexts;
    const std::string preamble = read_file(preamble_path);
    if (preamble.empty()) return contexts;
    const fs::path dir = fs::path(preamble_path).parent_path();
    const std::string stem = fs::path(preamble_path).stem().string();   // <unit>_preamble

    for (const auto& path : g_pair_headers) {
        const auto sites = g_inclusion_sites.find(path);
        if (sites == g_inclusion_sites.end()) continue;
        const std::string rel = header_mirror_relpath(path, inc_dirs);
        for (size_t n = 1; n <= sites->second.size(); ++n) {
            // What the n-th #include line reads in the preamble: the name as written for the
            // first, the copy's path for the others (apply_preamble_include_rewrites()).
            const std::string spelled =
                n == 1 ? sites->second[0].second
                       : inclusion_mirror_relpath(rel, static_cast<unsigned>(n));
            const unsigned occurrence =
                n == 1 ? include_occurrence_before(source, sites->second[0].first, spelled) : 0;
            unsigned seen = 0;
            size_t cut = std::string::npos, line_start = 0, pos = 0;
            while (pos < preamble.size()) {
                size_t eol = preamble.find('\n', pos);
                if (eol == std::string::npos) eol = preamble.size();
                if (is_include_of(preamble.substr(pos, eol - pos), spelled) &&
                    seen++ == occurrence) {
                    cut = eol < preamble.size() ? eol + 1 : eol;
                    line_start = pos;
                    break;
                }
                pos = eol + 1;
            }
            const std::string key = inclusion_key(path, static_cast<unsigned>(n));
            if (cut == std::string::npos) {
                if (verbose)
                    out << "[auto-split] no #include line for " << key
                        << " in the preamble; its pieces get the whole preamble\n";
                continue;
            }
            const std::string name =
                stem + "." + sanitize_filename(rel) + "." + std::to_string(n) + ".h";
            const std::string before_name =
                stem + "." + sanitize_filename(rel) + "." + std::to_string(n) + ".before.h";
            auto write = [&](const std::string& file, const std::string& text) {
                if (!fs::exists(file) || read_file(file) != text) {
                    std::ofstream ofs(file);
                    if (ofs.is_open()) ofs << text;
                }
            };
            write((dir / name).string(), preamble.substr(0, cut));
            write((dir / before_name).string(), preamble.substr(0, line_start));
            contexts[key] = PairContext{name, before_name};
            if (verbose) out << "[auto-split] context of " << key << ": " << name << "\n";
        }
    }
    return contexts;
}

static void resolve_header_deps(CXTranslationUnit tu,
                                const HarvestMap& harvest,
                                const VarHarvestMap& var_harvest,
                                const std::set<std::string>& referenced,
                                const std::vector<std::string>& candidates,
                                const std::string& context_preamble,
                                // Per candidate key, the preamble cut after that inclusion,
                                // for a pair header's inclusions. TODO/44 (B).
                                const std::map<std::string, PairContext>& pair_contexts,
                                SplitResult& result,
                                const std::string& output_dir,
                                const std::vector<std::string>& all_flags,
                                const std::vector<std::string>& extra_flags,
                                bool parse_clean,
                                bool verbose,
                                std::ostream& out) {
    for (const auto& key : candidates) {
        if (verbose) out << "\n[auto-split] " << key << "\n";

        auto it = harvest.find(key);
        std::vector<FunctionInfo> fns =
            (it == harvest.end()) ? std::vector<FunctionInfo>() : it->second;
        auto vit = var_harvest.find(key);
        std::vector<VariableInfo> vars =
            (vit == var_harvest.end()) ? std::vector<VariableInfo>() : vit->second;

        // `path#n` is inclusion n of a header read more than once. TODO/44 (B).
        const auto decoded = decode_inclusion_key(key);
        const std::string& inc_path = decoded.first;
        const unsigned inclusion = decoded.second;

        const std::string src = read_file(inc_path);
        if (src.empty()) continue;

        // Variables count, not only functions. A header that defines a namespace-scope
        // variable and no function still has to be rewritten: the preamble includes it in the
        // ordinary way and every piece includes the preamble, so the definition is compiled
        // once per piece and `ld -r` rejects the copies --
        // `multiple definition of 'unsigned_overflow_base35'` on Boost.Spirit's uint_radix
        // test, whose header is nothing but `char const*` definitions. Rewriting it lets
        // prepare_variables() mark them `inline` and leave them where they are, which is the
        // only placement that works for a variable whose type cannot survive being moved.
        // TODO/32.
        if (fns.empty() && vars.empty()) {
            // Nothing to split, but record the decision so it is not reconsidered on every
            // invocation.
            const std::string rel = inclusion_mirror_relpath(
                header_mirror_relpath(inc_path, unit_include_dirs(extra_flags)), inclusion);
            const std::string unit_dir =
                (fs::path(split_include_root(output_dir)) / fs::path(rel).parent_path()).string();
            if (verbose)
                out << "No function or variable definitions found in " << key << "\n";
            write_skipped_header_manifest(unit_dir, fs::path(inc_path).filename().string(),
                                          inc_path, "no function or variable definitions");
            continue;
        }

        const auto ctx = pair_contexts.find(key);
        SplitResult hdr_sr = split_unit(tu, inc_path, inc_path, src, fns, vars, referenced, true,
                                        output_dir, all_flags, extra_flags,
                                        ctx == pair_contexts.end() ? context_preamble
                                                                   : ctx->second.after,
                                        parse_clean, verbose, out, inclusion,
                                        ctx == pair_contexts.end() ? std::string()
                                                                   : ctx->second.before);
        if (!hdr_sr.success && verbose)
            out << "[auto-split] warning: failed to split " << key << "\n";
    }

    // The context preambles pieces actually include, split now or reused: those need a PCH.
    for (const auto& ctx : pair_contexts)
        if (g_split_headers.find(ctx.first) != g_split_headers.end())
            result.context_preambles.push_back((fs::path(output_dir) / ctx.second.after).string());

    const std::vector<std::string> includes = inclusions_of(tu);

    std::set<std::string> seen_dirs;
    std::map<std::string, unsigned> times_read;
    for (const auto& inc_path : includes) {
        // The n-th time a file is read, the copy split for its n-th inclusion, if any.
        const std::string key = inclusion_key(inc_path, ++times_read[inc_path]);
        auto it = g_split_headers.find(key);
        if (it != g_split_headers.end()) {
            if (verbose) out << "[header-dep] " << key << " -> " << it->second.split_dir << "\n";
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

    // A copy under include/_inclusion/<n>/ serves inclusion n of the header. TODO/44 (B).
    unsigned inclusion = 1;
    {
        std::error_code ec;
        const fs::path rel = fs::relative(manifest_path, include_root, ec);
        auto it = rel.begin();
        if (!ec && it != rel.end() && *it == "_inclusion" && ++it != rel.end()) {
            const std::string n = it->string();
            if (!n.empty() && std::all_of(n.begin(), n.end(), ::isdigit))
                inclusion = static_cast<unsigned>(std::stoul(n));
        }
    }

    // An empty list is how a header that was examined and deliberately not split is recorded.
    if (!abs_path.empty() && !info.compilable_files.empty())
        g_split_headers[inclusion_key(abs_path, inclusion)] = std::move(info);
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
// Probed once per set of include-path flags (include_path_flags()); the set of the unit
// being split is remembered so that is_stdlib_header() asks about the right toolchain.
static std::vector<std::string> g_include_path_flags;

static const std::vector<std::string>& cached_system_includes() {
    static std::map<std::string, std::vector<std::string>> by_flags;
    std::string key;
    for (const auto& f : g_include_path_flags) key += f + "\x1f";
    auto it = by_flags.find(key);
    if (it == by_flags.end())
        it = by_flags.emplace(key, g_compiler.empty()
                                       ? detect_system_includes("g++", g_include_path_flags)
                                       : detect_system_includes(g_compiler, g_include_path_flags))
                 .first;
    return it->second;
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
    g_include_path_flags = include_path_flags(extra_flags);
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
                             unsigned inclusion,
                             const std::string& preamble_filename,
                             const std::string& preamble_path,
                             const std::string& definitions_filename,
                             const std::string& definitions_context,
                             const std::string& definitions_variant,
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
    // Where each split-out definition came from, so that a later run can re-slice one body
    // without parsing. TODO/28.
    std::vector<HarvestDef> harvest;
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
        if (!input_is_header && g_module_unit.interface) {
            // An implementation unit of the module: the interface arrives through the BMI,
            // the global module fragment through the replay. TODO/43.
            if (!g_module_unit.gmf.empty())
                content << "module;\n#include \""
                        << fs::path(abs_path).stem().string() << "_preamble.h\"\n";
            content << "module " << g_module_unit.name << ";\n";
        } else {
            // The unit's imports, restated: an import inside the precompiled preamble does
            // not make the module visible here. TODO/43.
            for (const auto& imp : g_unit_imports) content << imp << "\n";
            if (!context_preamble.empty())
                content << "#include \"" << context_preamble << "\"\n";
            content << "#include \"" << preamble_filename << "\"\n";
        }
        // The definitions header carries what may exist in only one object, so exactly one
        // piece includes it. Which one does not matter; the first compilable one will do.
        if (!definitions_filename.empty() && !definitions_emitted && !should_keep_in_header(fn) &&
            definitions_variant.empty()) {
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

        // The linkage specification the definition was written under, restated: the
        // FunctionDecl's own extent starts after `extern "C"`, so the piece would otherwise
        // define a C++-linkage function beside the C-linkage declaration the preamble keeps.
        if (fn.c_linkage)
            body = "extern \"C\" {\n" + body + "\n}";

        if (!fn.scope_chain.empty()) {
            content << wrap_in_namespaces(body, fn.scope_chain) << "\n";
        } else {
            content << body << "\n";
        }

        {
            const size_t closed = conditionals_closed_by(fn.body);
            const size_t to_close =
                fn.conditionals.size() > closed ? fn.conditionals.size() - closed : 0;
            for (size_t ci = 0; ci < to_close; ++ci)
                content << "#endif\n";
        }

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
        // Only a definition whose body is carried verbatim somewhere can be re-sliced.
        // A definition whose extent is a macro invocation shared with other declarations
        // is left out: the gap hash covering it still proves it unchanged, and an edit to
        // it takes the slow path.
        //
        // A member *is* included even though it is emitted in a rebuilt out-of-line form.
        // The rebuild replaces the declarator -- `bool C::f()` for `bool f()` -- and
        // leaves the braces and everything in them exactly as written, so the body is
        // still verbatim and still the tail of what is emitted. Excluding members here
        // excluded almost every header definition worth re-slicing, side_info::collinear
        // among them.
        //
        // Kept definitions are recorded too, with no piece. This unit emitted nothing for
        // them -- nothing here needs an out-of-line copy -- so their body is carried by this
        // unit's rewritten copy of the file instead, and that copy is what a later edit
        // patches. Leaving them out was not merely a missed optimisation: a definition absent
        // from the harvest falls inside a *gap*, so an edit to its body reads as a change
        // outside every definition and the whole unit is re-split. On Boost.Spirit's suite,
        // where 194 units include the edited header and one emits the function, that was 267
        // re-splits against 1.
        const size_t rel_open = (!fn.shares_extent && !fn.macro_invocation)
                                    ? body_open_offset(fn.body)
                                    : std::string::npos;
        if (rel_open != std::string::npos) {
            // The body as it stands in the source, and -- for a piece -- where that same
            // text came to rest in it. rfind, because only the closing braces of the
            // enclosing namespaces follow it.
            const std::string body_region = fn.body.substr(rel_open);
            const size_t at = kept ? std::string::npos : new_content.rfind(body_region);
            if (kept || at != std::string::npos) {
                HarvestDef hd;
                hd.start = fn.start_offset;
                hd.end = fn.end_offset;
                hd.body_open = fn.start_offset + (unsigned)rel_open;
                hd.start_line = fn.start_line;
                hd.end_line = fn.end_line;
                if (!kept) {
                    hd.piece = out_path;
                    hd.piece_body_off = at;
                } else if (fn.declare_only) {
                    hd.piece = "=";   // declared in the copy: no piece, no body to patch
                }
                hd.body_hash = hash_bytes(body_region);
                hd.prefix_hash = hash_bytes(fn.body.substr(0, rel_open));
                harvest.push_back(hd);
            }
        }

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

        if (!kept) result.compilable_files.push_back(out_path);

        if (verbose) {
            out << "  [" << file_counter << "] " << fn.signature;
            if (kept) out << "  (header-only)";
            if (!needs_write) out << "  (unchanged)";
            out << "\n";
            out << "      Lines " << fn.start_line << "-" << fn.end_line
                << " -> " << out_path << "\n";
        }
    }

    write_harvest(output_dir, unit_tag, abs_path, read_file(abs_path), harvest);

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
        for (const auto& imp : g_unit_imports) content << imp << "\n";
        if (!definitions_variant.empty()) {
            // A pair header's inclusion: the context up to its #include line, then the
            // copy's variant with the definitions in place. A unit whose moved macro group
            // names a macro the unit undefines: its own variant, and nothing before it.
            if (!definitions_context.empty())
                content << "#include \"" << definitions_context << "\"\n";
            content << "#include \"" << definitions_variant << "\"\n";
        } else {
            if (!context_preamble.empty())
                content << "#include \"" << context_preamble << "\"\n";
            content << "#include \"" << definitions_filename << "\"\n";
        }
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
        // The manifest names the original, which is what the depfile has to name; which
        // inclusion it is for is in its path.
        g_split_headers[inclusion_key(abs_path, inclusion)] = std::move(hdr_info);
        write_header_manifest(output_dir, preamble_filename, abs_path, result.compilable_files);
        if (verbose) out << "Registered split header: " << inclusion_key(abs_path, inclusion) << " (" << result.compilable_files.size() << " compilable files)\n";
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

    size_t prefix_in_pch = 0;   // bytes of the source the prefix PCH already holds
    std::string prefix_path;    // the include block the prefix PCH was built from
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
        (void)prefix_in_pch;
        if (!prefix.empty()) {
            std::error_code ec;
            fs::create_directories(unit_dir, ec);
            prefix_path = (fs::path(unit_dir) / (unit_tag + "_prefix.h")).string();
            if (!fs::exists(prefix_path) || read_file(prefix_path) != prefix) {
                std::ofstream ofs(prefix_path);
                if (ofs.is_open()) ofs << prefix;
            }
            const std::string prefix_pch =
                build_libclang_pch(prefix_path, all_flags, verbose, out,
                                   fs::path(abs_path).parent_path().string());
            if (!prefix_pch.empty()) {
                parse_flags_vec.push_back("-include-pch");
                parse_flags_vec.push_back(prefix_pch);
                prefix_in_pch = prefix.size();
            }
        }
    }

    std::vector<const char*> args;
    for (const auto& f : parse_flags_vec)
        args.push_back(f.c_str());
    if (verbose) {
        // What libclang is given is not what the compiler was given, and when the two
        // disagree the split is decided on one program and compiled as another.
        std::cerr << "[cpp-splitter] libclang args:";
        for (const auto& f : parse_flags_vec) std::cerr << " " << f;
        std::cerr << "\n";
    }

    CXIndex index = clang_createIndex(0, 0);
    if (!index) {
        if (verbose) std::cerr << "Error: failed to create clang index\n";
        return result;
    }

    CXTranslationUnit tu = nullptr;
    // libclang's own preamble, which accelerates this parse and nothing else. It is not the
    // preamble PCH from build_pch(): that one is consumed by the compiler when building the
    // split pieces and is a separate, larger win.
    //
    // These two flags are expensive -- 2.28s against 1.43s to parse
    // libs/geometry/test/algorithms/area/area.cpp with its real flags, a 60% surcharge on
    // every parse, and the launcher parses the unit plus each header candidate. Since nothing
    // here ever calls clang_reparseTranslationUnit(), which is what a preamble is normally
    // for, removing them looks free and takes one unit's re-split from 12.2s to 8.3s.
    //
    // It is not free. header_split_candidates() builds its list from clang_getInclusions(),
    // and without a precompiled preamble -- with the prefix PCH supplying the include block
    // through -include-pch -- that call yields nothing usable: auto-splitting of headers stops
    // silently, the build still succeeds, and only launcher.depfile_names_originals and
    // launcher.header_edit_behind_pch notice. The flags stay until the candidate list comes
    // from somewhere cheaper. See TODO/29.
    // The detailed preprocessing record is what inclusions_of() reads: one
    // CXCursor_InclusionDirective per `#include` actually processed, with the file it
    // resolved to, whether the directive sits in the preamble region or after it, and once
    // per inclusion when a header is included twice. clang_getInclusions() gives neither of
    // the last two under a precompiled preamble.
    unsigned parse_flags = CXTranslationUnit_PrecompiledPreamble
                         | CXTranslationUnit_CreatePreambleOnFirstParse
                         | CXTranslationUnit_DetailedPreprocessingRecord;
    // When the prefix PCH is in use the include block is already in the translation unit,
    // and reading it again from the source processes every unguarded header twice: OpenCV's
    // *.simd.hpp, included on purpose twice under two macro states, came back a third and a
    // fourth time as `redefinition of ...`. The parse is therefore given the source with the
    // prefix blanked -- every character but the newlines replaced by a space, so that every
    // offset and line number the harvest records is the file's own.
    std::string parse_source;
    std::vector<CXUnsavedFile> unsaved;
    if (prefix_in_pch > 0 && prefix_in_pch <= source.size()) {
        parse_source = source;
        for (size_t i = 0; i < prefix_in_pch; ++i)
            if (parse_source[i] != '\n') parse_source[i] = ' ';
    }
    // A module interface unit is parsed as an ordinary translation unit, its module syntax
    // blanked: see blank_module_syntax() for why libclang cannot be given it as written.
    if (!input_is_header && g_module_unit.interface)
        parse_source = blank_module_syntax(parse_source.empty() ? source : parse_source);
    if (!parse_source.empty())
        unsaved.push_back({abs_path.c_str(), parse_source.c_str(),
                           static_cast<unsigned long>(parse_source.size())});
    CXErrorCode err = clang_parseTranslationUnit2(
        index, abs_path.c_str(), args.data(),
        static_cast<int>(args.size()), unsaved.empty() ? nullptr : unsaved.data(),
        static_cast<unsigned>(unsaved.size()), parse_flags, &tu);

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
    g_unit_main_file = fs::absolute(abs_path).lexically_normal().string();

    // One walk of this translation unit yields the inventory for the file itself and for
    // every header it should split, each seen in the context its includer establishes.
    // Deciding the header set first keeps the walk from recording functions nobody wants.
    g_pair_headers.clear();
    g_file_bases.clear();
    g_preamble_include_rewrites.clear();
    g_include_spellings.clear();
    g_mirror_rel_cache.clear();
    g_verbatim_mirrors.clear();
    if (!input_is_header) {
        std::error_code ec;
        collect_inclusion_sites(tu, fs::path(abs_path).lexically_normal().string(),
                                prefix_path.empty()
                                    ? std::string()
                                    : fs::absolute(prefix_path, ec).lexically_normal().string());
    }
    // A module interface unit splits no headers: its pieces are implementation units, which
    // include nothing of the unit's but the global module fragment. TODO/43.
    std::vector<std::string> candidates =
        (input_is_header || g_module_unit.interface)
            ? std::vector<std::string>()
            : header_split_candidates(tu, abs_path, output_dir, extra_flags, verbose, out);

    auto decline_for_header = [&]() {
        g_declined = true;
        std::cerr << "[cpp-splitter] not splitting " << input_path << ": "
                  << g_unsplittable_header
                  << " is included more than once with no include guard and defines a"
                     " function with external linkage, and "
                  << g_unsplittable_header_reason
                  << "; left unsplit it would reach every piece\n";
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
    };
    if (!g_unsplittable_header.empty()) {
        decline_for_header();
        return result;
    }

    std::set<std::string> wanted;
    for (const auto& c : candidates) wanted.insert(decode_inclusion_key(c).first);
    wanted.insert(abs_path);

    HarvestMap harvest;
    VarHarvestMap var_harvest;
    VisitorData vd{tu, &wanted, &harvest, &var_harvest};
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, &vd);

    if (!g_pair_headers.empty()) {
        attribute_pair_inclusions(harvest, var_harvest, candidates,
                                  files_with_external_definitions(tu), abs_path, source,
                                  unit_include_dirs(extra_flags), output_dir, verbose, out);
        if (!g_unsplittable_header.empty()) {
            decline_for_header();
            return result;
        }
    }

    std::vector<FunctionInfo> functions;
    {
        auto hit = harvest.find(abs_path);
        if (hit != harvest.end()) functions = hit->second;
    }

    std::set<std::string> referenced;
    collect_emitted(tu, abs_path, referenced);
    declare_only_candidates(harvest, inclusions_of(tu),
                            fs::path(abs_path).lexically_normal().string(), referenced);

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
        std::map<std::string, PairContext> pair_contexts;
        if (!g_pair_headers.empty())
            pair_contexts = write_pair_context_preambles(
                (fs::path(output_dir) / tu_preamble).string(), source,
                unit_include_dirs(extra_flags), verbose, out);
        resolve_header_deps(tu, harvest, var_harvest, referenced, candidates, tu_preamble,
                            pair_contexts, result, output_dir,
                            all_flags, extra_flags, parse_errors == 0, verbose, out);
        mirror_includers_of_split_headers(output_dir, unit_include_dirs(extra_flags),
                                          fs::path(abs_path).lexically_normal().string(),
                                          verbose, out);
        // Once every copy that is going to exist does, and not before. TODO/31.
        fix_mirror_quoted_includes(output_dir, unit_include_dirs(extra_flags), verbose, out);
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
    ofs << sr.context_preambles.size() << "\n";
    for (const auto& f : sr.context_preambles) ofs << f << "\n";
}

static bool read_split_cache(const std::string& split_dir, const std::string& hash,
                             SplitResult& sr) {
    if (hash.empty()) return false;
    std::ifstream ifs((fs::path(split_dir) / "split.cache").string());
    if (!ifs.is_open()) return false;

    // Start from nothing. The lists below are appended to, and this is called more than once
    // per run on the path where one candidate cache is tried and rejected before another is
    // read -- which listed every piece twice and made `ld -r` report every symbol as multiply
    // defined, in the one and only object that defined it.
    sr = SplitResult{};

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
    // Written since TODO/44 (B); a cache from before has no list here.
    if (!read_list(sr.context_preambles)) sr.context_preambles.clear();
    sr.success = true;
    return true;
}


// The previous split result, whatever inputs it was written for. The incremental path needs
// it precisely when the hash no longer matches, which is what read_split_cache() refuses.
static bool read_split_cache_any(const std::string& split_dir, SplitResult& sr) {
    std::ifstream ifs((fs::path(split_dir) / "split.cache").string());
    std::string stored;
    if (!ifs.is_open() || !std::getline(ifs, stored) || stored.empty()) return false;
    return read_split_cache(split_dir, stored, sr);
}

// Re-slice one edited body instead of re-splitting the unit. TODO/28.
//
// Called only when split_inputs_hash() missed, which means *something* changed. This decides
// whether that something was one function body and, if so, patches the two things such an
// edit actually affects -- the piece for that body, and the line numbers recorded after it --
// leaving everything else exactly as the previous run wrote it.
//
// The order matters: everything is verified before anything is written. A half-patched split
// directory would be worse than a slow one, and the guards below are cheap next to the parse
// they avoid.
static bool try_incremental_split(const std::string& split_dir,
                                  const std::string& input_file,
                                  const std::vector<std::string>& split_flags,
                                  SplitResult& sr, bool verbose) {
    (void)split_flags;
    (void)sr;
    auto refuse = [&](const char* why) {
        if (verbose) std::cerr << "[cpp-splitter] full split: " << why << "\n";
        return false;
    };

    if (std::getenv("CPP_SPLITTER_NO_INCREMENTAL_SPLIT"))
        return refuse("CPP_SPLITTER_NO_INCREMENTAL_SPLIT is set");

    const std::vector<std::string> changed = changed_prerequisites(split_dir);
    if (changed.size() == 1 && !changed.front().empty() && changed.front()[0] == '<')
        return refuse("there is no record of what the previous run read");
    if (changed.empty())
        return refuse("nothing changed, yet the hash missed");
    if (changed.size() != 1) {
        if (verbose)
            for (const auto& c : changed) std::cerr << "[cpp-splitter]   changed: " << c << "\n";
        return refuse("more than one input changed");
    }
    const std::string& file = changed.front();

    // The harvest for that file, among the ones this unit wrote: one for the unit itself and
    // one for every header it split.
    HarvestFile hf;
    bool found = false;
    std::string harvest_dir;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(split_dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->path().extension() != ".harvest") continue;
        HarvestFile candidate;
        if (read_harvest(it->path(), candidate) && candidate.path == file) {
            // A pair header has one harvest per inclusion, and a piece of either may hold
            // the edit. TODO/44 (B).
            if (found) return refuse("the changed file was split once per inclusion");
            hf = candidate;
            // Where this file's split output was written: its pieces, its `.keeps`, and -- for
            // a definition no piece was emitted for -- this unit's rewritten copy of the file.
            // Taken from the harvest rather than from a piece's path, because the edited
            // definition may be one of the kept ones and have no piece.
            harvest_dir = it->path().parent_path().string();
            found = true;
        }
    }
    if (!found) return refuse("the changed file has no harvest record");
    if (hf.defs.empty()) return refuse("the changed file contributed no split-out definition");
    // A definitions variant carries the kept definitions a second time, and the re-slice
    // patches only the copy; the full split rewrites both. TODO/47.
    for (fs::directory_iterator it(harvest_dir, ec), end; it != end && !ec; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() > 14 && name.compare(name.size() - 14, 14, ".definitions.h") == 0)
            return refuse("the file's definitions are compiled from a variant of it");
    }

    const std::string now = read_file(file);
    if (now.empty()) return refuse("the changed file cannot be read");
    const long delta = (long)now.size() - (long)hf.size;

    // A macro the file retracts decides whether a definition may be moved at all
    // (TODO/16, TODO/25). If that set moved, placement may have moved with it.
    if (undefined_macros_hash(now) != hf.undef_hash)
        return refuse("the set of macros the file undefines changed");

    // Locate the edit: assume it is inside definition k, then require every other segment of
    // the file to hash exactly as it did -- unshifted before k, shifted by delta after it.
    // A hypothesis that survives that is not a guess.
    auto seg = [&](size_t off, size_t len) -> std::string {
        if (off > now.size() || off + len > now.size()) return std::string("\x01<oob>");
        return now.substr(off, len);
    };
    size_t hit = hf.defs.size();
    for (size_t k = 0; k < hf.defs.size(); ++k) {
        bool ok = true;
        unsigned cursor = 0;
        for (size_t i = 0; i <= hf.defs.size() && ok; ++i) {
            const long shift = (i <= k) ? 0 : delta;
            const unsigned gap_end = (i < hf.defs.size()) ? hf.defs[i].start : (unsigned)hf.size;
            if (hash_bytes(seg((size_t)((long)cursor + shift), gap_end - cursor)) != hf.gaps[i])
                ok = false;
            if (i < hf.defs.size()) {
                const auto& d = hf.defs[i];
                if (i != k) {
                    const long dshift = (i < k) ? 0 : delta;
                    if (hash_bytes(seg((size_t)((long)d.start + dshift), d.end - d.start)) !=
                        d.extent_hash)
                        ok = false;
                }
                cursor = d.end;
            }
        }
        if (ok) {
            if (hit != hf.defs.size()) return refuse("the edit could not be located uniquely");
            hit = k;
        }
    }
    if (hit == hf.defs.size()) return refuse("the change is not confined to one definition");

    const HarvestDef& d = hf.defs[hit];
    const size_t new_end = (size_t)((long)d.end + delta);
    if (new_end > now.size() || new_end < d.body_open)
        return refuse("the edited definition no longer fits the file");

    // The signature must not have moved. Everything the preamble declares, and every other
    // piece's view of this function, was built from it and none of that is regenerated here.
    // Checking only that the opening brace is still at the same offset is not enough: an edit
    // that keeps the signature the same length would slip through.
    if (hash_bytes(now.substr(d.start, d.body_open - d.start)) != d.prefix_hash)
        return refuse("the definition's signature changed");

    const std::string new_extent = now.substr(d.start, new_end - d.start);
    const size_t new_open_rel = body_open_offset(new_extent);
    if (new_open_rel == std::string::npos || d.start + new_open_rel != d.body_open)
        return refuse("the new body is not a single balanced block");

    const std::string new_body = now.substr(d.body_open, new_end - d.body_open);

    // A body that gains or loses a preprocessor directive can change what the rest of the
    // file means, and #define/#undef inside one is exactly the case TODO/16 and TODO/25 were
    // about. Cheap to check, so check it rather than reason about it.
    {
        std::istringstream ls(blank_code_noise(new_body));
        std::string l;
        while (std::getline(ls, l)) {
            const size_t at = l.find_first_not_of(" \t");
            if (at != std::string::npos && l[at] == '#')
                return refuse("the new body contains a preprocessor directive");
        }
    }

    const std::string keeps_path =
        (fs::path(harvest_dir) / (fs::path(file).filename().string() + ".keeps")).string();
    // How far every line after this definition moved. The old body's line count is not
    // recoverable from a hash, so it is taken from the recorded span instead: a definition
    // beginning on start_line and spanning N newlines ends on start_line + N.
    const long shift = ((long)d.start_line + (long)count_newlines(new_extent)) -
                       (long)d.end_line;

    // Everything is proven; now write. The body's new text goes in first, because it is the
    // only content (rather than line numbering) that changes.
    //
    // Where it goes depends on whether this unit emitted a piece for the definition. If it did
    // not -- nothing here needed an out-of-line copy -- the body is carried by this unit's
    // rewritten copy of the file, and that copy is what has to be patched. The copy sits beside
    // the harvest under the same name as the original; for a definition kept out of the unit's
    // own source, it is the preamble.
    if (d.piece == "=") {
        // Declared only in this unit's copy: the copy has no body to patch, no piece
        // carries one, and the PCH built from the copy is still exact. The edit changes
        // nothing here (TODO/48).
        if (verbose)
            std::cerr << "[cpp-splitter] re-sliced nothing: this unit declares the edited"
                         " definition and does not emit it\n";
    } else if (d.piece.empty()) {
        const std::string prefix = now.substr(d.start, d.body_open - d.start);
        const size_t len = d.end - d.body_open;
        const std::string stem = fs::path(file).stem().string();
        const std::vector<std::string> candidates = {
            (fs::path(harvest_dir) / fs::path(file).filename()).string(),
            (fs::path(harvest_dir) / (stem + "_preamble.h")).string(),
        };

        std::string target, updated;
        for (const auto& cand : candidates) {
            const std::string text = read_file(cand);
            if (text.empty()) continue;
            const size_t q = text.find(prefix);
            if (q == std::string::npos) continue;
            if (text.find(prefix, q + 1) != std::string::npos)
                return refuse("the kept definition's signature appears twice in its copy");
            const size_t body_at = q + prefix.size();
            if (body_at + len > text.size()) continue;
            // The recorded length says where the old body ends, and the recorded hash proves
            // it: no brace matching, and a wrong guess refuses rather than corrupts.
            if (hash_bytes(text.substr(body_at, len)) != d.body_hash) continue;
            if (!target.empty())
                return refuse("more than one copy carries the kept definition");
            target = cand;
            updated = text.substr(0, body_at) + new_body + text.substr(body_at + len);
        }
        if (target.empty())
            return refuse("no copy of the file carries the kept definition as harvested");
        std::ofstream ofs(target);
        if (!ofs.is_open()) return refuse("the kept definition's copy cannot be written");
        ofs << updated;
    } else {
        const std::string piece = read_file(d.piece);
        const size_t len = d.end - d.body_open;
        if (piece.size() < d.piece_body_off + len)
            return refuse("the piece no longer matches its harvest record");
        if (hash_bytes(piece.substr(d.piece_body_off, len)) != d.body_hash)
            return refuse("the piece's body is not the one that was harvested");
        std::string updated = piece.substr(0, d.piece_body_off) + new_body +
                              piece.substr(d.piece_body_off + len);
        // The comment naming the source lines is part of the piece a full split would write,
        // so it has to move too or the outputs are not identical.
        const std::string old_span = "(lines " + std::to_string(d.start_line) + "-" +
                                     std::to_string(d.end_line) + ")";
        const std::string new_span = "(lines " + std::to_string(d.start_line) + "-" +
                                     std::to_string(d.end_line + shift) + ")";
        const size_t sp = updated.find(old_span);
        if (sp == std::string::npos) return refuse("the piece has no source-line comment");
        updated = updated.substr(0, sp) + new_span + updated.substr(sp + old_span.size());
        std::ofstream ofs(d.piece);
        if (!ofs.is_open()) return refuse("the piece cannot be written");
        ofs << updated;
    }

    // A definition kept in the preamble can sit *inside* the edited one -- a member of a
    // class declared local to the body. Whether its line moved depends on where in the body
    // the edit landed, which is more than the recorded span can answer, so refuse rather than
    // shift it wrongly. Checked before anything is written.
    //
    // Strictly inside: an entry recorded at the edited definition's own start line is not a
    // definition nested in its body. It is usually that definition itself, since a kept one
    // appears both here and in the harvest, and reading it as nested made the fast path refuse
    // its own work -- on 112 of Boost.Spirit's units. Anything else sharing that line sits at
    // or before the signature, so the edit below it does not move it either.
    if (shift != 0 && fs::exists(keeps_path)) {
        std::istringstream ks(read_file(keeps_path));
        std::string l;
        while (std::getline(ks, l)) {
            const size_t t1 = l.find('\t');
            const size_t t2 = (t1 == std::string::npos) ? t1 : l.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            long n = 0;
            try { n = std::stol(l.substr(t1 + 1, t2 - t1 - 1)); } catch (...) { continue; }
            if (n > (long)d.start_line && n <= (long)d.end_line)
                return refuse("a definition kept in the preamble sits inside the edited body");
        }
    }

    // Every other piece from this file, and every keeps entry, sits at a line number that
    // moved by `shift`.
    if (shift != 0) {
        for (size_t i = 0; i < hf.defs.size(); ++i) {
            if (i == hit) continue;
            const auto& o = hf.defs[i];
            if (o.start < d.start) continue;      // before the edit: nothing moved
            if (o.piece.empty()) continue;        // kept: no piece, nothing to renumber
            std::string piece = read_file(o.piece);
            if (piece.empty()) continue;
            const std::string old_span = "(lines " + std::to_string(o.start_line) + "-" +
                                         std::to_string(o.end_line) + ")";
            const std::string new_span = "(lines " + std::to_string(o.start_line + shift) +
                                         "-" + std::to_string(o.end_line + shift) + ")";
            const std::string old_line = "#line " + std::to_string(o.start_line) + " ";
            const std::string new_line = "#line " + std::to_string(o.start_line + shift) + " ";
            size_t at = piece.find(old_span);
            if (at != std::string::npos)
                piece = piece.substr(0, at) + new_span + piece.substr(at + old_span.size());
            at = piece.find(old_line);
            if (at != std::string::npos)
                piece = piece.substr(0, at) + new_line + piece.substr(at + old_line.size());
            std::ofstream ofs(o.piece);
            if (ofs.is_open()) ofs << piece;
        }

        if (fs::exists(keeps_path)) {
            std::istringstream ks(read_file(keeps_path));
            std::ostringstream out;
            std::string l;
            while (std::getline(ks, l)) {
                const size_t t1 = l.find('\t');
                const size_t t2 = (t1 == std::string::npos) ? t1 : l.find('\t', t1 + 1);
                long n = 0;
                if (t2 != std::string::npos) {
                    try { n = std::stol(l.substr(t1 + 1, t2 - t1 - 1)); } catch (...) { n = 0; }
                }
                if (n > (long)d.end_line)
                    out << l.substr(0, t1 + 1) << (n + shift) << l.substr(t2) << "\n";
                else
                    out << l << "\n";
            }
            std::ofstream ofs(keeps_path);
            if (ofs.is_open()) ofs << out.str();
        }
    }

    // Re-record the harvest against the file as it now is, so the next edit starts from the
    // truth rather than from where things used to be.
    {
        std::vector<HarvestDef> next;
        for (size_t i = 0; i < hf.defs.size(); ++i) {
            HarvestDef o = hf.defs[i];
            if (i == hit) {
                o.end = (unsigned)new_end;
                o.end_line = (unsigned)((long)o.end_line + shift);
            } else if (o.start >= d.start) {
                o.start = (unsigned)((long)o.start + delta);
                o.end = (unsigned)((long)o.end + delta);
                o.body_open = (unsigned)((long)o.body_open + delta);
                o.start_line = (unsigned)((long)o.start_line + shift);
                o.end_line = (unsigned)((long)o.end_line + shift);
            }
            o.body_hash = hash_bytes(now.substr(o.body_open, o.end - o.body_open));
            o.prefix_hash = hash_bytes(now.substr(o.start, o.body_open - o.start));
            next.push_back(o);
        }
        write_harvest(harvest_dir, fs::path(file).filename().string(), file, now, next);
    }

    // The cache and the input hashes are deliberately not written here. They are written by
    // the caller once the depfile has been rewritten for this build; writing them now would
    // record the prerequisites the *previous* run saw, and the split would then be judged
    // current against the wrong list.
    if (verbose)
        std::cerr << "[cpp-splitter] one body changed in " << file
                  << "; re-sliced its piece without parsing\n";
    return true;
}

// The linker used to combine a unit's pieces back into one object.
//
// Splitting turns one link of a dozen objects into one of several hundred, so which linker
// does the combining stops being an incidental choice. `CPP_SPLITTER_LINKER` names the
// program; it defaults to `ld` and is passed `-r` either way, since mold, lld and GNU ld all
// spell relocatable output the same. Set it to `mold` or `ld.lld` to try another.
// Where a bare program name resolves on PATH, or empty if it does not resolve at all.
//
// Needed twice, and for the same underlying reason: the tipi drivers exec what they are given
// rather than going through a shell, so they need an absolute path -- and a build can have the
// compiler driver without the linker driver, in which case asking for the latter has to be a
// question rather than an assumption.
static std::string which_on_path(const std::string& name) {
    if (name.find('/') != std::string::npos)
        return fs::exists(name) ? name : std::string();
    const char* path_env = std::getenv("PATH");
    const std::string path = path_env ? path_env : "";
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t sep = path.find(':', pos);
        const std::string dir =
            path.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
        std::error_code ec;
        if (!dir.empty()) {
            const fs::path cand = fs::path(dir) / name;
            // Executable, not merely present. tipibuild/tipi-ubuntu-2404 ships every
            // tipi-*-driver as -rwxrw-r-- owned by `tipi`, so a build running as any other
            // uid finds the file and cannot run it -- and a link driven at it dies with
            // "Permission denied", falls the unit back, and writes no split cache (TODO/31).
            if (fs::exists(cand, ec) && !fs::is_directory(cand, ec) &&
                ::access(cand.c_str(), X_OK) == 0)
                return cand.string();
        }
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    return std::string();
}

static std::string relocatable_linker() {
    const char* env = std::getenv("CPP_SPLITTER_LINKER");
    if (env && *env) return env;
    return "ld";
}


// ---------------------------------------------------------------------------------------
// TODO/35: perform the split on the Remote Build Execution cluster.
//
// A distributed split build sends every piece to the cluster and does the splitting here: one
// libclang parse per translation unit, several thousand pieces written, and an `ld -r` each.
// Measured on Boost.Spirit that local half is what a full build costs once the cache is warm --
// 456s against the ordinary build's 32s, with nothing compiled on either side. This moves the
// parse and the emit to the cluster too.
//
// Four mechanisms make it work, and every one was verified against the real cluster before any
// of this was written, because the design rests on them:
//
//   -labels=type=compile,compiler=clang,lang=cpp
//       reproxy's own C++ input processor scans the command line and computes the header
//       closure. We do not enumerate inputs -- the same scanner that decides what a piece
//       compile needs decides what the split needs, so the two cannot disagree.
//   -remote_wrapper=<this binary>
//       the scanned command stays a genuine compiler invocation, which is what the input
//       processor expects, and the splitter is put in front of it on the worker. Confirmed
//       that scanning still happens with a wrapper present: a header named nowhere in
//       -inputs was found and uploaded.
//   -output_directories=<split dir>
//       piece names cannot be known before the parse, but the directory holding them is
//       `<object>.o.split` and always was. Confirmed the whole tree comes home.
//   -env_var_allowlist=CPP_SPLITTER_EMIT_ONLY
//       -remote_wrapper takes a path and no arguments, so the mode travels in the
//       environment. Confirmed it arrives.
//
// EngFlow also rejects an action with no container image, so -platform has to carry one; the
// value is whatever cmake-re already put in RBE_platform.
//
// The remote step is emit-only on purpose. If it compiled and linked as well, a unit would
// become a single action and a one-line edit inside one function would invalidate all of it --
// which is the opposite of the property being protected: today such an edit executes one remote
// compile where an ordinary build executes 271.
static bool remote_split_enabled() {
    const char* v = std::getenv("CPP_SPLITTER_REMOTE_SPLIT");
    return v && std::string(v) == "1" && !std::getenv("CPP_SPLITTER_EMIT_ONLY");
}

// Everything the rewrapper invocation needs, or nothing. Each value comes from what cmake-re
// already exports for tipi-compiler-driver, so there is no second configuration to keep in
// step -- and a missing one means this build is not talking to a cluster, which is a reason to
// split locally rather than an error.
struct RemoteSplitEnv {
    std::string rewrapper;
    std::string server_address;
    std::string exec_root;
    std::string platform;
    bool usable() const {
        return !rewrapper.empty() && !server_address.empty() && !exec_root.empty() &&
               !platform.empty();
    }
};

static RemoteSplitEnv remote_split_env() {
    RemoteSplitEnv e;
    auto env = [](const char* n) {
        const char* v = std::getenv(n);
        return v ? std::string(v) : std::string();
    };
    e.rewrapper = which_on_path("rewrapper");
    if (e.rewrapper.empty()) {
        const std::string home = env("TIPI_HOME_DIR");
        const std::string base = home.empty() ? "/usr/local/share/.tipi" : home;
        std::error_code ec;
        if (fs::is_directory(base + "/reclient", ec))
            for (const auto& d : fs::directory_iterator(base + "/reclient", ec)) {
                const std::string cand = (d.path() / "rewrapper").string();
                if (fs::exists(cand, ec) && ::access(cand.c_str(), X_OK) == 0) {
                    e.rewrapper = cand;
                    break;
                }
            }
    }
    e.server_address = env("RBE_server_address");
    e.exec_root = env("RBE_exec_root");
    e.platform = env("RBE_platform");
    return e;
}

// A path as rewrapper wants it: relative to the exec root. Returns empty when the path lies
// outside, which is the one case this cannot express -- the caller then splits locally.
static std::string exec_root_relative(const std::string& path, const std::string& exec_root) {
    std::error_code ec;
    const fs::path abs = fs::absolute(path).lexically_normal();
    const fs::path root = fs::absolute(exec_root).lexically_normal();
    const fs::path rel = abs.lexically_relative(root);
    if (rel.empty() || rel.native().rfind("..", 0) == 0) return std::string();
    return rel.string();
}

// Ask the cluster to produce the split. Returns false for every reason not to have tried, and
// the caller then splits here; the only thing it must never do is report success without the
// split tree actually being present.
static bool try_remote_split(const std::string& real_compiler,
                             const std::vector<std::string>& compile_flags,
                             const std::string& input_file,
                             const std::string& output_file,
                             const std::string& split_dir,
                             SplitResult& sr,
                             bool verbose) {
    auto decline = [&](const char* why) {
        if (verbose) std::cerr << "[cpp-splitter] local split: " << why << "\n";
        return false;
    };

    const RemoteSplitEnv env = remote_split_env();
    if (!env.usable()) return decline("no RBE environment (RBE_server_address/exec_root/platform)");

    std::error_code self_ec;
    std::string self = fs::read_symlink("/proc/self/exe", self_ec).string();
    if (self.empty()) return decline("cannot determine this binary's own path");

    // Two different relativisations of the same file, because rewrapper documents two:
    // -remote_wrapper is "relative to the current working directory of rewrapper", while
    // -toolchain_inputs is "relative to the exec root". Passing the exec-root path for both
    // made the worker die in execvp() -- it resolved the wrapper against the action's working
    // directory, where nothing of that name exists.
    //
    // So stage the binary inside the working directory (which is itself under the exec root,
    // being cmake-re's build tree) and name it each way. Keyed by content hash: written once,
    // safe against concurrent launchers, and recognised by the cluster's cache as the same
    // input across every unit of a build rather than re-uploaded per unit.
    //
    // libclang is not staged with it. The worker runs the image that
    // environments/ubuntu-clang.pkr.js pins by digest, and the binary's RPATH points into that
    // image's toolchain, so the library is already there at the same absolute path.
    const fs::path cwd = fs::current_path(self_ec);
    if (exec_root_relative(self, env.exec_root).empty()) {
        const std::string hash = file_content_hash(self);
        if (hash.empty()) return decline("cannot hash this binary");
        const fs::path staged = cwd / ".cpp-splitter" / hash / "cpp-splitter";
        if (!fs::exists(staged, self_ec)) {
            fs::create_directories(staged.parent_path(), self_ec);
            const fs::path tmp = staged.string() + ".tmp." + std::to_string(::getpid());
            fs::copy_file(self, tmp, fs::copy_options::overwrite_existing, self_ec);
            if (self_ec) return decline("cannot stage this binary under the working directory");
            fs::permissions(tmp,
                            fs::perms::owner_all | fs::perms::group_read |
                                fs::perms::group_exec | fs::perms::others_read |
                                fs::perms::others_exec,
                            self_ec);
            fs::rename(tmp, staged, self_ec);   // atomic: a loser of the race sees a whole file
            if (self_ec) fs::remove(tmp, self_ec);
        }
        if (!fs::exists(staged, self_ec))
            return decline("cannot stage this binary under the working directory");
        self = staged.string();
    }

    const std::string self_cwd_rel =
        fs::absolute(self).lexically_normal().lexically_relative(cwd.lexically_normal()).string();
    const std::string self_rel = exec_root_relative(self, env.exec_root);
    const std::string split_rel = exec_root_relative(split_dir, env.exec_root);
    const std::string obj_rel = exec_root_relative(output_file, env.exec_root);
    if (self_rel.empty()) {
        if (verbose)
            std::cerr << "[cpp-splitter] local split: this binary (" << self
                      << ") is not under the exec root (" << env.exec_root << ")\n";
        return false;
    }
    if (split_rel.empty() || obj_rel.empty()) return decline("the output is not under the exec root");

    // The split directory has to exist before the action runs: the remote wrapper writes into
    // it, and an output directory that never appears is an error rather than an empty result.
    // The directory has to be emptied first, and this is not housekeeping. reclient *merges*
    // an -output_directories result into whatever is already there, so a split directory left
    // over from an earlier run keeps every piece the new split did not happen to overwrite.
    // Piece names carry an index, so a unit that gained or lost a definition ends up with two
    // generations of pieces at once, and `ld -r` then reports `multiple definition of main`.
    // On the `one body` row of Boost.Spirit's suite that was 265 of 279 units falling back to
    // a plain compile.
    if (fs::exists(split_dir, self_ec)) {
        fs::remove_all(split_dir, self_ec);
        if (self_ec) return decline("cannot clear the existing split directory");
    }
    fs::create_directories(split_dir, self_ec);

    std::string cmd = shell_quote(env.rewrapper);
    cmd += " -server_address " + shell_quote(env.server_address);
    cmd += " -exec_root " + shell_quote(env.exec_root);
    cmd += " -labels=type=compile,compiler=clang,lang=cpp";
    cmd += " -exec_strategy=remote";
    cmd += " -remote_wrapper=" + shell_quote(self_cwd_rel);
    cmd += " -toolchain_inputs=" + shell_quote(self_rel);
    cmd += " -output_directories=" + shell_quote(split_rel);
    cmd += " -env_var_allowlist=CPP_SPLITTER_EMIT_ONLY";
    cmd += " -platform=" + shell_quote(env.platform);
    cmd += " --";
    cmd += " " + shell_quote(real_compiler);
    for (const auto& f : compile_flags) cmd += " " + shell_quote(f);
    cmd += " -c -o " + shell_quote(output_file) + " " + shell_quote(input_file);

    // Kept rather than swallowed: when a remote action fails the reason is in rewrapper's
    // stderr, and "the remote split failed" on its own is not a diagnosis.
    const std::string rw_log = (fs::path(split_dir) / "remote-split.log").string();
    cmd += " > " + shell_quote(rw_log) + " 2>&1";

    if (verbose) std::cerr << "[cpp-splitter] remote split: " << cmd << "\n";
    setenv("CPP_SPLITTER_EMIT_ONLY", "1", 1);
    const int rc = run_command_quiet(cmd);
    unsetenv("CPP_SPLITTER_EMIT_ONLY");
    if (rc != 0) {
        std::cerr << "[cpp-splitter] remote split failed (" << rc << "), splitting locally."
                     " rewrapper said:\n";
        std::ifstream rl(rw_log);
        std::string line;
        int shown = 0;
        while (std::getline(rl, line) && shown < 8) {
            std::cerr << "    " << line << "\n";
            ++shown;
        }
        return false;
    }

    // What came back has to be a split, not an empty directory. read_split_cache() checks that
    // every file it names exists, so this is also the check that the download was complete.
    if (!read_split_cache(split_dir, "remote-emit", sr))
        return decline("the remote split returned no usable result");
    if (verbose)
        std::cerr << "[cpp-splitter] remote split: " << sr.compilable_files.size()
                  << " piece(s) returned from the cluster\n";
    return true;
}

// `@file` on the command line: the file's whitespace-separated tokens in its place. CMake
// hands a module unit its flags this way (`@foo.cxx.o.modmap`: `-x c++-module
// -fmodule-output=…`), and the launcher has to see them to know it is compiling a module
// unit and where the BMI is expected. TODO/43.
static std::vector<std::string> expand_response_files(int argc, char* argv[], int from) {
    std::vector<std::string> args;
    for (int i = from; i < argc; ++i) {
        const std::string arg = argv[i];
        std::error_code ec;
        if (arg.size() > 1 && arg[0] == '@' && fs::is_regular_file(arg.substr(1), ec)) {
            std::istringstream in(read_file(arg.substr(1)));
            std::string token;
            while (in >> token) {
                // CMake writes one plain token per line; a quoted one is unwrapped.
                if (token.size() >= 2 && (token.front() == '"' || token.front() == '\'') &&
                    token.back() == token.front())
                    token = token.substr(1, token.size() - 2);
                args.push_back(token);
            }
            continue;
        }
        args.push_back(arg);
    }
    return args;
}

// The flags that make a compile a module-unit compile, taken out: neither the libclang
// parse nor an implementation-unit piece may carry them. `-fmodule-file=` stays: importers
// and pieces alike need the BMIs. TODO/43.
static std::vector<std::string> without_module_output_flags(const std::vector<std::string>& flags) {
    std::vector<std::string> kept;
    for (size_t i = 0; i < flags.size(); ++i) {
        const std::string& f = flags[i];
        if (f == "-x" && i + 1 < flags.size() && flags[i + 1] == "c++-module") { ++i; continue; }
        if (f == "-xc++-module") continue;
        if (f.rfind("-fmodule-output", 0) == 0) continue;
        if (f == "-fmodules-reduced-bmi" || f == "-fexperimental-modules-reduced-bmi") continue;
        kept.push_back(f);
    }
    return kept;
}

// The BMIs a compile reads: every `-fmodule-file=[name=]path`.
static std::vector<std::string> module_files_of(const std::vector<std::string>& flags) {
    std::vector<std::string> pcms;
    for (const auto& f : flags) {
        if (f.rfind("-fmodule-file=", 0) != 0) continue;
        std::string value = f.substr(14);
        const size_t eq = value.find('=');
        if (eq != std::string::npos) value = value.substr(eq + 1);
        if (!value.empty()) pcms.push_back(value);
    }
    return pcms;
}

// Whether any BMI a unit's pieces compile against has changed since they were compiled.
//
// Not by timestamp: the launcher touches an unchanged BMI so that the build system's
// dependency check stays quiet, so a timestamp says nothing. `modules.hash` beside the
// pieces records the content hash of each BMI they were last compiled against. TODO/43.
static std::string module_files_hash_text(const std::vector<std::string>& pcms) {
    std::string text;
    for (const auto& pcm : pcms) {
        const std::string h = file_content_hash(pcm);
        text += (h.empty() ? std::string("<absent>") : h) + " " + pcm + "\n";
    }
    return text;
}

static int run_as_launcher(int argc, char* argv[]) {
    bool verbose = launcher_verbose();
    std::string compiler = argv[1];

    // What the system-include and default-standard probes run against.
    //
    // Not necessarily argv[1]. cmake-re composes CMAKE_CXX_COMPILER_LAUNCHER as
    // "<cpp-splitter>;tipi-compiler-driver", so this process is invoked as
    // `cpp-splitter tipi-compiler-driver clang++ <flags>` and argv[1] is a launcher. Probing
    // *it* runs `tipi-compiler-driver -x c++ -E -dM /dev/null`, which the driver answers by
    // trying to exec `-x` as the compiler and dying:
    //
    //     execve failed: No such file or directory
    //
    // Both probes then return nothing, so the parse gets no system include paths and no
    // probed standard. libclang parses something that is not the translation unit the
    // compiler sees, the harvest comes out of a broken AST, and the damage surfaces far away
    // -- `inline` inserted into the middle of an alias template in boost/mp11/algorithm.hpp,
    // five Boost.Spirit units falling back, and a defect report about alias templates that
    // had nothing to do with alias templates. See TODO/34.
    //
    // Probed lazily, once, the first time flags are built for a parse.
    g_compiler = (compiler == "tipi-compiler-driver" && argc > 2) ? argv[2] : compiler;

    std::string input_file;
    std::string output_file;
    bool has_c_flag = false;

    bool has_md = false;
    bool has_mmd = false;
    std::string mf_path;
    std::string mt_target;

    std::vector<std::string> other_flags;

    const std::vector<std::string> args = expand_response_files(argc, argv, 2);
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];
        if (arg == "-c") {
            has_c_flag = true;
        } else if (arg == "-o" && i + 1 < args.size()) {
            output_file = args[++i];
        } else if (arg == "-MD") {
            has_md = true;
        } else if (arg == "-MMD") {
            has_mmd = true;
        } else if (arg == "-MF" && i + 1 < args.size()) {
            mf_path = args[++i];
        } else if (arg == "-MT" && i + 1 < args.size()) {
            mt_target = args[++i];
        } else if (arg == "-MQ" && i + 1 < args.size()) {
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

    // A C++20 module unit. The interface unit is split into the interface the compiler
    // precompiles and one implementation unit per body; an implementation unit is compiled
    // whole for now; an importer is an ordinary unit whose pieces restate its imports.
    // TODO/43.
    {
        const std::string source = read_file(input_file);
        g_module_unit = detect_module_unit(source);
        g_unit_imports = unit_import_lines(source);
    }
    // The module-unit flags as the build gave them, for the interface's own compile.
    std::vector<std::string> interface_only_flags;
    for (size_t i = 0; i < other_flags.size(); ++i) {
        const std::string& f = other_flags[i];
        if (f == "-x" && i + 1 < other_flags.size() && other_flags[i + 1] == "c++-module") {
            interface_only_flags.push_back(f); interface_only_flags.push_back(other_flags[++i]);
        } else if (f == "-xc++-module" || f == "-fmodules-reduced-bmi" ||
                   f == "-fexperimental-modules-reduced-bmi") {
            interface_only_flags.push_back(f);
        }
    }
    bool interface_from_x_flag = false;
    for (size_t i = 0; i + 1 < other_flags.size(); ++i)
        if (other_flags[i] == "-x" && other_flags[i + 1] == "c++-module") interface_from_x_flag = true;
    if (g_module_unit.implementation || (interface_from_x_flag && !g_module_unit.interface)) {
        std::string cmd;
        for (int i = 1; i < argc; i++) {
            if (i > 1) cmd += " ";
            cmd += shell_quote(argv[i]);
        }
        std::cerr << "[cpp-splitter] declined: " << input_file
                  << " is a module implementation unit, compiling whole\n";
        if (verbose) std::cerr << "[cpp-splitter] passthrough: " << cmd << "\n";
        return run_command_quiet(cmd);
    }
    const bool module_interface = g_module_unit.interface;
    // Where the build expects the BMI: `-fmodule-output=<path>`, or beside the object.
    std::string planned_pcm;
    if (module_interface) {
        for (const auto& f : other_flags)
            if (f.rfind("-fmodule-output=", 0) == 0) planned_pcm = f.substr(16);
        if (planned_pcm.empty() && !output_file.empty())
            planned_pcm = fs::path(output_file).replace_extension(".pcm").string();
        if (planned_pcm.empty()) planned_pcm = g_module_unit.name + ".pcm";
        // What the pieces and the parse see: the compile of an ordinary translation unit.
        other_flags = without_module_output_flags(other_flags);
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

    // Being chained behind another launcher.
    //
    // cmake-re composes CMAKE_CXX_COMPILER_LAUNCHER as "<cpp-splitter>;tipi-compiler-driver",
    // so this process is invoked as `cpp-splitter tipi-compiler-driver clang++ <flags>` and
    // argv[1] is a launcher rather than a compiler. The real compiler is the argument after
    // it, and the two have to stay adjacent in everything re-invoked: a driver handed
    // `-I/some/dir` where it expects the compiler treats it as its own flag, prints its usage
    // and exits non-zero. That is what made every unit of a distributed Boost.Spirit build
    // fall back -- 279 fallbacks over 277 units, each one silently compiled whole.
    //
    // compile_prefix is therefore what a command starts with, and compile_flags is what is
    // left once the compiler has been taken out of the flags.
    const bool chained_behind_driver = (compiler == "tipi-compiler-driver") && !other_flags.empty();
    std::string compile_prefix = shell_quote(compiler);
    std::vector<std::string> compile_flags = other_flags;
    if (chained_behind_driver) {
        compile_prefix += " " + shell_quote(compile_flags.front());
        compile_flags.erase(compile_flags.begin());
    }
    
    const std::string inputs_hash = split_inputs_hash(split_dir, input_file, split_flags);

    SplitResult sr;
    // Four ways to arrive at a split, in increasing order of what they cost: reuse what is
    // there, re-slice one body out of the recorded harvest, ask the cluster to parse, parse
    // here.
    const bool inputs_unchanged = read_split_cache(split_dir, inputs_hash, sr);
    bool re_sliced = false;
    if (inputs_unchanged) {
        if (verbose)
            std::cerr << "[cpp-splitter] inputs unchanged, reusing the existing split\n";
        for (const auto& hdr : sr.header_obj_dirs) (void)hdr;
    } else if (read_split_cache_any(split_dir, sr) &&
               try_incremental_split(split_dir, input_file, split_flags, sr, verbose)) {
        // One function body changed and its piece has been re-sliced from the recorded
        // harvest. Nothing was parsed, but the split *did* change, so the cache and the input
        // hashes below still have to be rewritten -- and they have to be written after
        // rewrite_depfile(), or they record the prerequisites of the run before this one.
        //
        // This is tried before the remote split, and the order is load-bearing rather than a
        // preference. Re-slicing costs no parse and no network, and it rewrites only the one
        // piece whose body moved -- so every other piece of the unit keeps its content and its
        // action key, and the build system has nothing to recompile. Asking the cluster first
        // regenerates the whole split instead: measured on the `one body` row of
        // benchmark-spirit-cmake-re.sh, that turned 1 remote compile into 4836 and the row from
        // 77.6s into 431.6s, because all 194 units that include the edited header came back
        // with fresh pieces.
        re_sliced = true;
    } else if (remote_split_enabled() && chained_behind_driver &&
               // chained_behind_driver guarantees other_flags is non-empty and that its first
               // element is the real compiler; compile_flags is what remains.
               try_remote_split(other_flags.front(), compile_flags,
                                input_file, output_file, split_dir, sr, verbose)) {
        // The cluster produced the pieces. Everything below -- compiling them, linking them --
        // is unchanged and still happens as separate actions, which is what keeps a one-line
        // edit costing one compile instead of a whole unit.
        re_sliced = false;
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

    // Emit-only, which is how this process runs on a worker: produce the pieces, record what
    // was produced, and stop. Compiling and linking them is the caller's job, back on the
    // machine that asked -- see try_remote_split() for why that separation is the whole point.
    if (std::getenv("CPP_SPLITTER_EMIT_ONLY")) {
        if (!sr.success) {
            std::cerr << "cpp-splitter: --emit-only split failed for " << input_file << "\n";
            return 1;
        }
        write_split_cache(split_dir, "remote-emit", sr);
        if (verbose)
            std::cerr << "[cpp-splitter] emit-only: " << sr.compilable_files.size()
                      << " piece(s) written, nothing compiled\n";
        return 0;
    }

    if (!sr.success) {
        // Not gated on verbose. A fallback means the tool did nothing it exists to do, and
        // the build succeeds either way -- so a silent one is a silent regression. The
        // failure that motivated this said so: a stale prefix PCH made every header edit
        // fall back, and the only trace was in a log nobody was reading.
        // A decline is not a fallback. The reason was printed where it was decided; here it
        // only says the unit is compiled whole, in words the benchmarks do not count as a
        // fallback -- that column is for defects.
        if (g_declined)
            std::cerr << "[cpp-splitter] declined: compiling whole\n";
        else
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

    // The BMIs this unit's pieces compile against -- the ones it imports, and for an
    // interface unit its own -- and whether any changed since the pieces were last
    // compiled. Content, not timestamp: see module_files_hash_text(). TODO/43.
    std::vector<std::string> piece_pcms = module_files_of(compile_flags);
    if (module_interface) piece_pcms.push_back(planned_pcm);
    const std::string modules_hash_path = (fs::path(split_dir) / "modules.hash").string();
    auto pcms_changed_now = [&]() {
        return !piece_pcms.empty() &&
               read_file(modules_hash_path) != module_files_hash_text(piece_pcms);
    };
    bool pcms_changed = !module_interface && pcms_changed_now();
    if (pcms_changed) {
        // A PCH built against the old BMI is not one clang will load against the new.
        std::error_code ec;
        fs::remove_all(sr.preamble_filename + ".gch", ec);
        for (const auto& ctx : sr.context_preambles) fs::remove_all(ctx + ".gch", ec);
        if (verbose) std::cerr << "[cpp-splitter] a BMI changed: pieces and PCH rebuilt\n";
    }

    bool tipi_compiler_driver_in_use = false;
    if (module_interface) {
      // No PCH: the "preamble" is the interface unit itself, compiled below.
      tipi_compiler_driver_in_use = (compiler == "tipi-compiler-driver");
    } else if (compiler == "tipi-compiler-driver") {
      std::cout << "BEGIN compiler_with_driver is: " << std::endl;
      tipi_compiler_driver_in_use = true;
      auto pch_flags = other_flags;
      auto actual_compiler = *pch_flags.begin();
      pch_flags = std::vector<std::string>(pch_flags.begin()+1, pch_flags.end());
      std::cout << "compiler_with_driver is: " << compiler << " " << actual_compiler<< std::endl;
      pch_flags.insert(pch_flags.begin(),
                       {"-I" + split_include_root(split_dir),
                        "-I" + fs::absolute(input_file).parent_path().string()});
      build_pch(sr.preamble_filename, actual_compiler, "", pch_flags, split_dir, verbose, std::cerr);
      for (const auto& ctx : sr.context_preambles)
          build_pch(ctx, actual_compiler, "", pch_flags, split_dir, verbose, std::cerr);
    } else {
      auto pch_flags = other_flags;
      pch_flags.insert(pch_flags.begin(),
                       {"-I" + split_include_root(split_dir),
                        "-I" + fs::absolute(input_file).parent_path().string()});
      build_pch(sr.preamble_filename, compiler, "", pch_flags, split_dir, verbose, std::cerr);
      for (const auto& ctx : sr.context_preambles)
          build_pch(ctx, compiler, "", pch_flags, split_dir, verbose, std::cerr);
    }

    // Whether the dependency-tracking flags have been handed to some compile yet.
    bool dep_flags_placed = false;
    std::vector<std::string> obj_files;
    std::vector<CompileJob> parallel_jobs;
    int launcher_skipped = 0;
    bool split_build_failed = false;

    // The interface unit's own compile: the rewritten interface, with the module flags the
    // build gave, writes the BMI and the interface object. The BMI lands beside the pieces
    // first and replaces the one the build planned only when it differs -- so an edit to a
    // body, which leaves the interface text and so the BMI byte-identical, changes no
    // digest an importer's action or cache key is made of. It is touched otherwise, since
    // the build system judges the edge by the timestamps of its outputs. TODO/43.
    bool interface_recompiled = false;
    if (module_interface) {
        const std::string iface_src = sr.preamble_filename;
        const std::string iface_obj =
            (fs::path(split_dir) / (fs::path(iface_src).stem().string() + ".o")).string();
        const std::string local_pcm =
            (fs::path(split_dir) / (g_module_unit.name + ".pcm")).string();
        obj_files.push_back(iface_obj);
        // The dependency flags are the interface compile's, whether or not it runs: a piece
        // given them lists itself as a prerequisite, and the next body edit then reads as
        // two changed inputs and loses the re-slice. With no compile writing the depfile,
        // rewrite_depfile() restores the cached one.
        dep_flags_placed = true;
        if (needs_recompile(iface_src, iface_obj) || !fs::exists(local_pcm)) {
            std::string cmd = compile_prefix;
            cmd += " -I" + shell_quote(split_dir);
            cmd += " -I" + shell_quote(fs::absolute(input_file).parent_path().string());
            for (const auto& f : compile_flags) cmd += " " + shell_quote(f);
            for (const auto& f : interface_only_flags) cmd += " " + shell_quote(f);
            cmd += " -fmodule-output=" + shell_quote(local_pcm);
            if (has_md || has_mmd) {
                cmd += has_mmd ? " -MMD" : " -MD";
                if (!mf_path.empty()) cmd += " -MF " + shell_quote(mf_path);
                const std::string mt = mt_target.empty() ? output_file : mt_target;
                if (!mt.empty()) cmd += " -MT " + shell_quote(mt);
            }
            cmd += " -c -o " + shell_quote(iface_obj) + " " + shell_quote(iface_src);
            if (verbose) std::cerr << "[cpp-splitter] compile (interface): " << cmd << "\n";
            const int ret = run_command_quiet(cmd);
            if (ret != 0) {
                std::cerr << "cpp-splitter: compilation failed for the module interface: "
                          << iface_src << "\n";
                split_build_failed = true;
            } else {
                interface_recompiled = true;
            }
        } else if (verbose) {
            std::cerr << "[cpp-splitter] up-to-date: " << iface_src << "\n";
        }
        if (!split_build_failed) {
            std::error_code ec;
            fs::create_directories(fs::path(planned_pcm).parent_path(), ec);
            const bool differs = !fs::exists(planned_pcm) ||
                                 file_content_hash(planned_pcm) != file_content_hash(local_pcm);
            if (differs) {
                fs::copy_file(local_pcm, planned_pcm, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    std::cerr << "cpp-splitter: cannot write the BMI to " << planned_pcm << "\n";
                    split_build_failed = true;
                }
                if (verbose) std::cerr << "[cpp-splitter] BMI written: " << planned_pcm << "\n";
            } else {
                fs::last_write_time(planned_pcm, fs::file_time_type::clock::now(), ec);
                if (verbose)
                    std::cerr << "[cpp-splitter] BMI unchanged: " << planned_pcm
                              << " (timestamp updated)\n";
            }
            pcms_changed = pcms_changed_now();
        }
    }

    for (size_t fi = 0; fi < sr.compilable_files.size(); ++fi) {
        const auto& cpp = sr.compilable_files[fi];
        std::string obj = cpp.substr(0, cpp.size() - 4) + ".o";
        obj_files.push_back(obj);

        if (split_build_failed) break;
        if (!needs_recompile(cpp, obj, sr.preamble_filename) && !pcms_changed) {
            if (verbose) std::cerr << "[cpp-splitter] up-to-date: " << cpp << "\n";
            ++launcher_skipped;
            continue;
        }

        std::string cmd = compile_prefix;

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

        for (const auto& f : compile_flags)
            cmd += " " + shell_quote(f);

        // The interface unit's compile took the dependency flags when there is one.
        const bool place_deps = fi == 0 && (has_md || has_mmd) && !dep_flags_placed;
        if (place_deps) {
            dep_flags_placed = true;
            cmd += has_mmd ? " -MMD" : " -MD";
            if (!mf_path.empty())
                cmd += " -MF " + shell_quote(mf_path);
            std::string mt = mt_target.empty() ? output_file : mt_target;
            if (!mt.empty())
                cmd += " -MT " + shell_quote(mt);
        }

        if (module_interface)
            // An implementation unit of the module, against the BMI the build reads.
            cmd += " -fmodule-file=" + shell_quote(g_module_unit.name + "=" + planned_pcm);
        else
            cmd += pch_include_flag(cpp, split_dir);
        cmd += " -c -o " + shell_quote(obj) + " " + shell_quote(cpp);

        if (place_deps) {
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
        auto results = compile_parallel(parallel_jobs, tipi_compiler_driver_in_use, verbose, std::cerr);
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
            // Against the context preamble the piece includes first, whose PCH is rebuilt
            // when a header copy changes; without it a header piece kept its object
            // across such a change.
            if (!fs::exists(hobj) || pcms_changed ||
                needs_recompile(hcpp, hobj, piece_context_header(hcpp, split_dir))) {
                std::string cmd = compile_prefix;
                // The unit's own preamble lives at the root of the split directory, and the
                // header pieces include it for context. The split tree precedes the
                // project's own include directories for the reason above.
                cmd += " -I" + shell_quote(split_dir);
                cmd += " -I" + shell_quote(split_include_root(split_dir));
                for (const auto& hdr_dir : sr.header_obj_dirs)
                    cmd += " -I" + shell_quote(hdr_dir);
                cmd += " -I" + shell_quote(fs::absolute(input_file).parent_path().string());
                for (const auto& f : compile_flags)
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

                cmd += pch_include_flag(hcpp, split_dir);
                cmd += " -c -o " + shell_quote(hobj) + " " + shell_quote(hcpp);
                hdr_compile_jobs.push_back({cmd, hcpp, hobj});
            }
            obj_files.push_back(hobj);
            if (verbose) std::cerr << "[cpp-splitter] header dep .o: " << hobj << "\n";
        }
        if (!hdr_compile_jobs.empty()) {
            if (verbose) std::cerr << "[cpp-splitter] compiling " << hdr_compile_jobs.size() << " header dep file(s)\n";
            auto hdr_results = compile_parallel(hdr_compile_jobs, tipi_compiler_driver_in_use, verbose, std::cerr);
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
        if (!piece_pcms.empty()) {
            std::ofstream ofs(modules_hash_path);
            if (ofs.is_open()) ofs << module_files_hash_text(piece_pcms);
        }
        if (!inputs_unchanged) {
            write_split_cache(split_dir,
                              split_inputs_hash(split_dir, input_file, split_flags), sr);
            // What each prerequisite hashed to this time, so that the next run can say which
            // one changed instead of only that something did. TODO/28.
            write_inputs_hashes(split_dir, input_file);
        }
        (void)re_sliced;
    }

    if (!split_build_failed) {
        bool need_link = (launcher_skipped < (int)sr.compilable_files.size()) || !sr.header_obj_files.empty() || !fs::exists(output_file) || interface_recompiled;

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

            // Hand the relocatable link to tipi-linker-driver when the compiles are already
            // going through tipi-compiler-driver.
            //
            // Chained behind cmake-re, every piece compiles as a distributed action while this
            // link stays local -- and it is not a small step: one `ld -r` per translation unit
            // over every object that unit produced, a few hundred of them on Boost.Spirit.
            // Measured on that suite, a body edit recompiles about the same number of remote
            // actions with the splitter as without (210 against 213), so what is left to win
            // is exactly the work that is not being distributed, and this is most of it.
            //
            // The driver takes the linker as its first argument, the way the compiler driver
            // takes the compiler, so the two stay adjacent for the same reason they do in a
            // piece's compile line.
            // Measured on the Boost.Spirit suite, this costs rather than saves: a body edit
            // went from 103.9s to 120s, and reclient recorded 272 more remote actions --
            // exactly the number of links. The objects are already here, so handing the link
            // to the cluster buys a round trip to upload a few hundred of them per unit.
            //
            // It is on by default anyway, because the local `ld -r` is the one part of a
            // distributed split build that does not distribute, and a link that goes through
            // the driver is cacheable where a local one is not -- which is worth more on a
            // machine with fewer cores than this one, or on a rebuild the cluster has already
            // seen. CPP_SPLITTER_NO_LINKER_DRIVER=1 turns it off; benchmarks/ has the numbers.
            // Asking whether the linker driver is there, rather than assuming it. A build can
            // be chained behind the compiler driver without the linker driver being on PATH --
            // every test fixture that stubs the former is exactly that case -- and driving the
            // link at a program that does not exist fails the link, which falls back the whole
            // unit and writes no split cache (TODO/31). Degrade to a plain `ld -r` instead.
            const std::string linker_driver =
                (chained_behind_driver && !std::getenv("CPP_SPLITTER_NO_LINKER_DRIVER"))
                    ? which_on_path("tipi-linker-driver")
                    : std::string();
            const bool driven_link = !linker_driver.empty();
            // Resolved to an absolute path before it is handed over. The driver execs the
            // linker itself rather than going through a shell, so a bare `ld` that PATH would
            // have found dies as `execve failed: No such file or directory`.
            const std::string driven_linker = driven_link ? which_on_path(linker) : linker;
            std::string cmd = driven_link
                                  ? shell_quote(linker_driver) + " " + shell_quote(driven_linker)
                                  : shell_quote(linker);
            cmd += " -r -o " + shell_quote(output_file);
            for (const auto& obj : obj_files)
                cmd += " " + shell_quote(obj);
            if (verbose)
                std::cerr << "[cpp-splitter] " << (driven_link ? "tipi-linker-driver " : "")
                          << linker << " -r: " << cmd << "\n";
            int ret = run_command_quiet(cmd);
            if (ret != 0) {
                std::cerr << "cpp-splitter: relocatable link failed using '" << linker
                          << "'\n";
                split_build_failed = true;
                // The cache and the input hashes were written before this link, and a run
                // that reuses them goes straight to the same link and fails the same way,
                // every time, without a parse in between to notice anything changed. A
                // split whose link failed is not a split to remember.
                std::error_code rm_ec;
                fs::remove(fs::path(split_dir) / "split.cache", rm_ec);
                fs::remove(fs::path(split_dir) / "inputs.hash", rm_ec);
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
        if (g_declined)
            std::cerr << "Warning: declined: compiling whole\n";
        else
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
        for (const auto& ctx : sr.context_preambles)
            build_pch(ctx, cxx_compiler, "", pch_flags, output_dir, true, std::cout);
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
            cmd += pch_include_flag(cpp_file, output_dir);
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
            auto results = compile_parallel(jobs, false /* no tipi_compiler_driver_in_use in basic mode */, true);
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
                auto hdr_results = compile_parallel(hdr_jobs, false /* no tipi_compiler_driver_in_use in basic mode */, true);
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
