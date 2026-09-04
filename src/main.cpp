#include <clang-c/Index.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
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
    std::vector<ScopeEntry> scope_chain;
    bool is_template;
    bool is_static;

    // Set by prepare_functions() once the whole file has been visited.
    bool defined_in_class = false;    // body written inside the class, not already out-of-line
    unsigned unnamed_ns_depth = 0;    // innermost consecutive unnamed namespaces around it
    bool in_class_template = false;   // member of a class template: cannot go out-of-line
    bool in_anonymous_class = false;  // no class name to qualify a definition with
    bool keep_in_header = false;      // definition has to stay in the preamble
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
    return result;
}

struct VisitorData {
    CXTranslationUnit tu;
    const std::string* source;
    std::vector<FunctionInfo>* functions;
    const std::string* filename;
};

static CXChildVisitResult visitor(CXCursor cursor, CXCursor /*parent*/, CXClientData data) {
    auto* vd = static_cast<VisitorData*>(data);

    CXSourceLocation loc = clang_getCursorLocation(cursor);
    if (clang_Location_isInSystemHeader(loc))
        return CXChildVisit_Continue;

    CXFile cursor_file;
    clang_getFileLocation(loc, &cursor_file, nullptr, nullptr, nullptr);
    if (!cursor_file)
        return CXChildVisit_Continue;

    std::string cursor_filename = cx_to_string(clang_getFileName(cursor_file));
    if (cursor_filename != *vd->filename)
        return CXChildVisit_Continue;

    CXCursorKind kind = clang_getCursorKind(cursor);

    bool is_function_def = false;
    if (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod ||
        kind == CXCursor_Constructor || kind == CXCursor_Destructor ||
        kind == CXCursor_FunctionTemplate) {
        is_function_def = clang_isCursorDefinition(cursor);
    }

    if (!is_function_def) {
        if (kind == CXCursor_Namespace || kind == CXCursor_ClassDecl ||
            kind == CXCursor_StructDecl || kind == CXCursor_ClassTemplate) {
            return CXChildVisit_Recurse;
        }
        return CXChildVisit_Continue;
    }

    FunctionInfo info;
    info.name = cx_to_string(clang_getCursorSpelling(cursor));
    info.is_template = (kind == CXCursor_FunctionTemplate);
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
            }
            if (pname.empty()) {
                // An unnamed namespace contributes no name to qualify with, but the split
                // definition is hoisted out of it, so the count is needed to place the
                // declaration in the same scope as the definition.
                if (sk == ScopeKind::Namespace && innermost)
                    ++info.unnamed_ns_depth;
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

    vd->functions->push_back(std::move(info));

    return CXChildVisit_Continue;
}

// Decided by prepare_functions(), which needs the source text and so cannot run here.
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

// Declaration left in place of a removed free-function definition. It is emitted at the
// position the definition occupied, so it inherits the surrounding namespaces and any #if
// context and needs no wrapping of its own. Emitting it here rather than appending it to
// the end of the preamble also means it precedes every use the original file had: a
// function pointer initialised at namespace scope just below the definition would
// otherwise refer to a name that has not been declared yet.
static std::string generate_forward_decl_inplace(const FunctionInfo& fn,
                                                 const std::string& stem) {
    // Members keep their declaration inside the class body instead.
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
    return sig + ";";
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
    decl += sig + ";";
    for (size_t i = 0; i < ns_names.size(); ++i)
        decl += " }";

    return decl;
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
                                               const std::string& unit_tag) {
    StaticRenameMap renames;
    std::set<std::string> seen;
    for (const auto& fn : functions) {
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

// Fills in the derived fields on every function: whether its definition has to stay in the
// header, and for class members the declaration left behind in the class plus the
// out-of-line definition written into the split file.
static void prepare_functions(std::vector<FunctionInfo>& functions) {
    for (auto& fn : functions) {
        std::vector<std::string> class_names;
        for (const auto& e : fn.scope_chain)
            if (e.kind == ScopeKind::Class) class_names.push_back(e.name);
        const bool is_member = !class_names.empty() && fn.defined_in_class;

        if (fn.is_template || fn.in_class_template || fn.in_anonymous_class) {
            fn.keep_in_header = true;
            continue;
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
        if (contains_decl_token(decl_blanked, "constexpr") ||
            contains_decl_token(decl_blanked, "consteval")) {
            fn.keep_in_header = true;
            continue;
        }

        if (!is_member) {
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

static std::string generate_preamble(const std::string& source,
                                     const std::vector<FunctionInfo>& functions,
                                     const std::string& stem,
                                     const std::string& source_path,
                                     const StaticRenameMap& renames) {
    struct Range {
        unsigned start, end;
        bool keep;
        const FunctionInfo* fn;
    };
    std::vector<Range> ranges;
    for (const auto& fn : functions) {
        ranges.push_back({fn.start_offset, fn.end_offset, should_keep_in_header(fn), &fn});
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const Range& a, const Range& b) {
                  if (a.start != b.start) return a.start < b.start;
                  if (a.end != b.end) return a.end < b.end;
                  return a.keep && !b.keep;   // a kept entry represents the group
              });

    // Several functions can share one source extent: a macro such as BOOST_BITMASK
    // expands to a whole set of operators, and every one of them reports the macro
    // invocation as its extent. Emitting the retained text once per function would repeat
    // the macro and redefine everything it declares, so collapse identical extents.
    ranges.erase(std::unique(ranges.begin(), ranges.end(),
                             [](const Range& a, const Range& b) {
                                 return a.start == b.start && a.end == b.end;
                             }),
                 ranges.end());

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
            preamble += apply_static_renames(source.substr(r.start, r.end - r.start), renames);
        } else if (r.fn) {
            // The definition moved to a split file, so a declaration has to take its
            // place: inside the class for a member, otherwise right here, where the
            // definition used to be.
            ensure_newline();
            if (!r.fn->member_decl.empty())
                preamble += apply_static_renames(r.fn->member_decl, renames) + ";";
            else if (r.fn->is_static) {
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
    for (const auto& fn : functions) {
        if (should_keep_in_header(fn) || fn.is_static)
            continue;   // renamed functions were already declared in place, above
        std::string decl = generate_forward_decl_wrapped(fn, stem, source);
        if (!decl.empty())
            preamble += decl + "\n";
    }

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
        if (verbose) out << "  libclang PCH up-to-date: " << pch_file << "\n";
        return pch_file;
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
    bool success;
    std::vector<std::string> header_obj_dirs;
    std::vector<std::string> header_obj_files;
};

struct SplitHeaderInfo {
    std::string split_dir;
    std::string preamble_path;
    std::vector<std::string> compilable_files;
};

static std::unordered_map<std::string, SplitHeaderInfo> g_split_headers;

struct CachedTU {
    CXIndex index = nullptr;
    CXTranslationUnit tu = nullptr;
    std::vector<std::string> flags;
    fs::file_time_type source_mtime;

    ~CachedTU() {
        if (tu) clang_disposeTranslationUnit(tu);
        if (index) clang_disposeIndex(index);
    }

    CachedTU() = default;
    CachedTU(CachedTU&& o) noexcept
        : index(o.index), tu(o.tu), flags(std::move(o.flags)), source_mtime(o.source_mtime)
    { o.index = nullptr; o.tu = nullptr; }
    CachedTU& operator=(CachedTU&& o) noexcept {
        if (this != &o) {
            if (tu) clang_disposeTranslationUnit(tu);
            if (index) clang_disposeIndex(index);
            index = o.index; tu = o.tu;
            flags = std::move(o.flags); source_mtime = o.source_mtime;
            o.index = nullptr; o.tu = nullptr;
        }
        return *this;
    }
    CachedTU(const CachedTU&) = delete;
    CachedTU& operator=(const CachedTU&) = delete;
};

static std::unordered_map<std::string, CachedTU> g_tu_cache;

static std::string default_socket_path() {
    const char* env = std::getenv("CPP_SPLITTER_SOCKET");
    if (env) return env;
    std::string path = "/tmp/cpp-splitter-" + std::to_string(getuid()) + ".sock";
    return path;
}

static bool send_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static bool recv_all(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        ssize_t n = ::recv(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static bool send_msg(int fd, const std::string& msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    if (!send_all(fd, &len, 4)) return false;
    if (len > 0 && !send_all(fd, msg.data(), len)) return false;
    return true;
}

static bool recv_msg(int fd, std::string& msg) {
    uint32_t len = 0;
    if (!recv_all(fd, &len, 4)) return false;
    if (len > 64 * 1024 * 1024) return false;
    msg.resize(len);
    if (len > 0 && !recv_all(fd, &msg[0], len)) return false;
    return true;
}

static std::string encode_request(const std::string& input,
                                   const std::string& output_dir,
                                   const std::vector<std::string>& flags,
                                   bool verbose) {
    std::ostringstream oss;
    oss << input << '\n' << output_dir << '\n' << (verbose ? "1" : "0") << '\n';
    oss << flags.size() << '\n';
    for (const auto& f : flags) oss << f << '\n';
    return oss.str();
}

struct DecodedRequest {
    std::string input;
    std::string output_dir;
    bool verbose;
    std::vector<std::string> flags;
};

static DecodedRequest decode_request(const std::string& msg) {
    DecodedRequest req;
    std::istringstream iss(msg);
    std::getline(iss, req.input);
    std::getline(iss, req.output_dir);
    std::string v; std::getline(iss, v);
    req.verbose = (v == "1");
    std::string n; std::getline(iss, n);
    int nflags = 0;
    try { nflags = std::stoi(n); } catch (...) {}
    for (int i = 0; i < nflags; ++i) {
        std::string f; std::getline(iss, f);
        req.flags.push_back(f);
    }
    return req;
}

static std::string encode_response(const SplitResult& sr, const std::string& output_text) {
    std::ostringstream oss;
    oss << (sr.success ? "1" : "0") << '\n';
    oss << sr.preamble_filename << '\n';
    oss << sr.compilable_files.size() << '\n';
    for (const auto& f : sr.compilable_files) oss << f << '\n';
    oss << sr.header_obj_dirs.size() << '\n';
    for (const auto& d : sr.header_obj_dirs) oss << d << '\n';
    oss << sr.header_obj_files.size() << '\n';
    for (const auto& f : sr.header_obj_files) oss << f << '\n';
    oss << output_text;
    return oss.str();
}

static SplitResult decode_response(const std::string& msg, std::string& output_text) {
    SplitResult sr;
    std::istringstream iss(msg);
    std::string s; std::getline(iss, s);
    sr.success = (s == "1");
    std::getline(iss, sr.preamble_filename);
    std::string n; std::getline(iss, n);
    int nfiles = 0;
    try { nfiles = std::stoi(n); } catch (...) {}
    for (int i = 0; i < nfiles; ++i) {
        std::string f; std::getline(iss, f);
        sr.compilable_files.push_back(f);
    }
    std::string nd; std::getline(iss, nd);
    int ndirs = 0;
    try { ndirs = std::stoi(nd); } catch (...) {}
    for (int i = 0; i < ndirs; ++i) {
        std::string d; std::getline(iss, d);
        sr.header_obj_dirs.push_back(d);
    }
    std::string no; std::getline(iss, no);
    int nobjs = 0;
    try { nobjs = std::stoi(no); } catch (...) {}
    for (int i = 0; i < nobjs; ++i) {
        std::string f; std::getline(iss, f);
        sr.header_obj_files.push_back(f);
    }
    std::ostringstream rest;
    rest << iss.rdbuf();
    output_text = rest.str();
    return sr;
}

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

static void resolve_header_deps(CXTranslationUnit tu,
                                 SplitResult& result,
                                 const std::string& output_dir,
                                 const std::vector<std::string>& extra_flags,
                                 bool verbose,
                                 std::ostream& out) {
    std::vector<std::string> includes;
    clang_getInclusions(tu, inclusion_visitor, &includes);

    bool do_auto_split = auto_include_split_enabled();

    const auto inc_dirs = include_dirs_from_flags(extra_flags);
    const std::string include_root = split_include_root(output_dir);

    std::set<std::string> seen_includes;
    for (const auto& inc_path : includes) {
        if (!do_auto_split) break;
        if (!seen_includes.insert(inc_path).second) continue;
        if (!is_header_file(inc_path)) continue;
        if (is_stdlib_header(inc_path)) continue;
        if (g_split_headers.find(inc_path) != g_split_headers.end()) continue;

        // Must match the path write_header_manifest() produces for this header, which
        // is the mirrored location plus ".split". The two used to disagree, so the
        // staleness check never found a manifest and every header was re-split on every
        // invocation.
        std::string manifest = (fs::path(include_root) /
            (header_mirror_relpath(inc_path, inc_dirs) + ".split")).string();
        bool stale = false;
        if (fs::exists(manifest)) {
            auto hdr_time = fs::last_write_time(inc_path);
            auto man_time = fs::last_write_time(manifest);
            stale = (hdr_time > man_time);
        }

        if (!fs::exists(manifest) || stale) {
            if (verbose) out << "\n[auto-split] " << inc_path << "\n";
            SplitResult hdr_sr = do_split(inc_path, output_dir, extra_flags, verbose, out);
            if (!hdr_sr.success && verbose)
                out << "[auto-split] warning: failed to split " << inc_path << "\n";
        }
    }

    std::set<std::string> seen_dirs;
    for (const auto& inc_path : includes) {
        auto it = g_split_headers.find(inc_path);
        if (it != g_split_headers.end()) {
            if (verbose) out << "[header-dep] " << inc_path << " -> " << it->second.split_dir << "\n";
            if (seen_dirs.insert(it->second.split_dir).second) {
                result.header_obj_dirs.push_back(it->second.split_dir);
            }
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

static void load_header_manifests(const std::string& output_dir) {
    if (!fs::exists(output_dir)) return;
    const std::string include_root = split_include_root(output_dir);
    // Manifests are nested under the mirrored include tree, so this has to recurse.
    for (const auto& entry : fs::recursive_directory_iterator(output_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        if (fname.size() < 6 || fname.substr(fname.size() - 6) != ".split") continue;

        std::ifstream ifs(entry.path());
        if (!ifs.is_open()) continue;

        std::string abs_path;
        std::getline(ifs, abs_path);
        std::string n;
        std::getline(ifs, n);
        int nfiles = 0;
        try { nfiles = std::stoi(n); } catch (...) {}

        SplitHeaderInfo info;
        info.split_dir = include_root;
        info.preamble_path =
            (entry.path().parent_path() / fname.substr(0, fname.size() - 6)).string();
        for (int i = 0; i < nfiles; ++i) {
            std::string f;
            std::getline(ifs, f);
            if (!f.empty()) info.compilable_files.push_back(f);
        }

        if (!abs_path.empty() && !info.compilable_files.empty()) {
            g_split_headers[abs_path] = std::move(info);
        }
    }
}

static SplitResult do_split_with_cache(const std::string& input_path,
                                        const std::string& output_dir,
                                        const std::vector<std::string>& extra_flags,
                                        bool verbose,
                                        std::ostream& out);

static SplitResult try_server_split(const std::string& input_path,
                                     const std::string& output_dir,
                                     const std::vector<std::string>& extra_flags,
                                     bool verbose,
                                     std::ostream& out) {
    SplitResult empty;
    empty.success = false;

    std::string sock_path = default_socket_path();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return empty;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return empty;
    }

    std::string req = encode_request(input_path, output_dir, extra_flags, verbose);
    if (!send_msg(fd, req)) {
        close(fd);
        return empty;
    }

    std::string resp;
    if (!recv_msg(fd, resp)) {
        close(fd);
        return empty;
    }
    close(fd);

    std::string output_text;
    SplitResult sr = decode_response(resp, output_text);
    if (!output_text.empty()) out << output_text;
    return sr;
}

static int g_server_fd = -1;
static std::string g_server_socket_path;

static void server_cleanup(int) {
    if (g_server_fd >= 0) close(g_server_fd);
    if (!g_server_socket_path.empty()) unlink(g_server_socket_path.c_str());
    _exit(0);
}

static int run_server(const std::string& sock_path) {
    g_server_socket_path = sock_path;
    unlink(sock_path.c_str());

    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        std::cerr << "Error: cannot create socket\n";
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(g_server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Error: cannot bind to " << sock_path << "\n";
        close(g_server_fd);
        return 1;
    }

    if (listen(g_server_fd, 5) < 0) {
        std::cerr << "Error: listen failed\n";
        close(g_server_fd);
        unlink(sock_path.c_str());
        return 1;
    }

    signal(SIGINT, server_cleanup);
    signal(SIGTERM, server_cleanup);
    signal(SIGPIPE, SIG_IGN);

    std::cerr << "[cpp-splitter server] listening on " << sock_path << "\n";
    std::cerr << "[cpp-splitter server] cached TUs: " << g_tu_cache.size() << "\n";

    while (true) {
        int client = accept(g_server_fd, nullptr, nullptr);
        if (client < 0) continue;

        std::string req_msg;
        if (!recv_msg(client, req_msg)) {
            close(client);
            continue;
        }

        auto req = decode_request(req_msg);
        if (req.verbose) {
            std::cerr << "[cpp-splitter server] received " << req.flags.size() << " flag(s):";
            for (const auto& f : req.flags) std::cerr << " " << f;
            std::cerr << "\n";
        }
        std::ostringstream capture;
        SplitResult sr = do_split_with_cache(req.input, req.output_dir, req.flags, req.verbose, capture);
        std::string resp = encode_response(sr, capture.str());
        send_msg(client, resp);
        close(client);

        std::cerr << "[cpp-splitter server] split " << req.input
                  << " -> " << sr.compilable_files.size() << " files"
                  << " (cached TUs: " << g_tu_cache.size() << ")\n";
    }
}

static const std::vector<std::string>& cached_system_includes() {
    static std::vector<std::string> includes = detect_system_includes();
    return includes;
}

static std::vector<std::string> build_clang_flags(const std::vector<std::string>& extra_flags,
                                                   bool force_cxx = false) {
    std::vector<std::string> all_flags = {"-std=c++17", "-fsyntax-only", "-Wno-everything"};
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

// Shared tail of do_split() and do_split_with_cache(): builds the PCH, writes one split
// .cpp per function, prunes outputs left over from a previous run, and registers header
// dependencies. Both callers reach this point with identical state; keeping it in one
// place is what stops the two paths from drifting apart.
static void emit_split_files(CXTranslationUnit tu,
                             const std::vector<FunctionInfo>& functions,
                             const std::string& input_path,
                             const std::string& abs_path,
                             const std::string& output_dir,
                             const std::string& include_root,
                             const std::string& unit_tag,
                             const std::string& preamble_filename,
                             const std::string& preamble_path,
                             const std::vector<std::string>& all_flags,
                             const std::vector<std::string>& extra_flags,
                             bool input_is_header,
                             bool parse_clean,
                             bool verbose,
                             std::ostream& out,
                             SplitResult& result) {
    build_libclang_pch(preamble_path, all_flags, verbose, out);

    if (verbose) out << "Found " << functions.size() << " function(s) in " << input_path << ":\n\n";

    const StaticRenameMap static_renames = build_static_rename_map(functions, unit_tag);

    std::vector<std::string> current_files;
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
            content << "// Note: template - kept in preamble header for compilation\n";
        content << "// ---\n\n";
        content << "#include \"" << preamble_filename << "\"\n\n";

        // Members are emitted in their out-of-line form; everything else as written.
        std::string body = fn.outlined_body.empty() ? fn.body : fn.outlined_body;
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
        if (!input_is_header)
            body = strip_decl_specifier(body, "inline");

        body = apply_static_renames(body, static_renames);

        std::string line_directive = "#line " + std::to_string(fn.start_line) +
                                     " \"" + abs_path + "\"\n";

        body = line_directive + body;
        if (input_is_header)
            body = "__attribute__((used))\n" + body;

        if (!fn.scope_chain.empty()) {
            content << wrap_in_namespaces(body, fn.scope_chain) << "\n";
        } else {
            content << body << "\n";
        }

        std::string new_content = content.str();
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

        bool kept = should_keep_in_header(fn);
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
    } else {
        resolve_header_deps(tu, result, output_dir, extra_flags, verbose, out);
    }
}


static SplitResult do_split_with_cache(const std::string& input_path,
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
    auto all_flags = build_clang_flags(extra_flags, is_header_file(abs_path));
    if (verbose) {
        out << "[server] libclang flags (" << all_flags.size() << "):";
        for (const auto& f : all_flags) out << " " << f;
        out << "\n";
    }

    bool input_is_header = is_header_file(abs_path);
    std::string preamble_filename = input_is_header
        ? fs::path(input_path).filename().string()
        : stem + "_preamble.h";

    // A header is mirrored under <split_dir>/include at the path it was included as; the
    // translation unit itself keeps the root of the split directory.
    std::string unit_dir = output_dir;
    if (input_is_header) {
        std::string rel = header_mirror_relpath(abs_path, include_dirs_from_flags(extra_flags));
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

    std::vector<std::string> parse_flags_vec = all_flags;
    std::string libclang_pch = build_libclang_pch(preamble_path, all_flags, verbose, out);
    if (!libclang_pch.empty()) {
        parse_flags_vec.push_back("-include-pch");
        parse_flags_vec.push_back(libclang_pch);
    }

    auto it = g_tu_cache.find(abs_path);
    bool cache_hit = false;
    CXTranslationUnit tu = nullptr;

    if (it != g_tu_cache.end()) {
        auto cur_mtime = fs::last_write_time(abs_path);
        if (it->second.flags == parse_flags_vec) {
            if (cur_mtime == it->second.source_mtime) {
                tu = it->second.tu;
                cache_hit = true;
                if (verbose) out << "[server] reusing cached TU (unchanged): " << abs_path << "\n";
            } else {
                if (verbose) out << "[server] reparsing (source changed): " << abs_path << "\n";
                int reparse_err = clang_reparseTranslationUnit(
                    it->second.tu, 0, nullptr, clang_defaultReparseOptions(it->second.tu));
                if (reparse_err == 0) {
                    tu = it->second.tu;
                    it->second.source_mtime = cur_mtime;
                    cache_hit = true;
                } else {
                    if (verbose) std::cerr << "[server] reparse failed, will parse fresh\n";
                    g_tu_cache.erase(it);
                }
            }
        } else {
            if (verbose) out << "[server] flags changed, reparsing fresh: " << abs_path << "\n";
            g_tu_cache.erase(it);
        }
    }

    if (!cache_hit) {
        CXIndex index = clang_createIndex(0, 0);
        if (!index) {
            if (verbose) std::cerr << "Error: failed to create clang index\n";
            return result;
        }

        std::vector<const char*> args;
        for (const auto& f : parse_flags_vec) args.push_back(f.c_str());

        unsigned parse_flags = 0;
        if (libclang_pch.empty())
            parse_flags = CXTranslationUnit_PrecompiledPreamble
                        | CXTranslationUnit_CreatePreambleOnFirstParse;
        CXErrorCode err = clang_parseTranslationUnit2(
            index, abs_path.c_str(), args.data(),
            static_cast<int>(args.size()), nullptr, 0, parse_flags, &tu);

        if (err != CXError_Success || !tu) {
            if (verbose) std::cerr << "Error: failed to parse translation unit (code: " << err << ")\n";
            clang_disposeIndex(index);
            return result;
        }

        CachedTU cached;
        cached.index = index;
        cached.tu = tu;
        cached.flags = parse_flags_vec;
        cached.source_mtime = fs::last_write_time(abs_path);
        g_tu_cache[abs_path] = std::move(cached);

        if (verbose) out << "[server] parsed and cached: " << abs_path << "\n";
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
        result.success = true;
        return result;
    }

    if (parse_errors > 0 && verbose) {
        // Not a header, so splitting continues; say so plainly rather than leaving the
        // reader to guess whether the output can be trusted.
        out << "Warning: " << parse_errors << " parse error(s) in " << input_path
            << "; splitting anyway, and no stale output will be pruned\n";
    }

    std::vector<FunctionInfo> functions;
    VisitorData vd{tu, &source, &functions, &abs_path};
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, &vd);
    prepare_functions(functions);

    if (functions.empty()) {
        if (verbose) out << "No function definitions found in " << input_path << "\n";
        result.success = true;
        if (!input_is_header) {
            resolve_header_deps(tu, result, output_dir, extra_flags, verbose, out);
        }
        return result;
    }

    if (!claim_output_path(preamble_path, abs_path))
        return result;

    fs::create_directories(unit_dir);

    result.preamble_filename = preamble_path;
    const StaticRenameMap static_renames = build_static_rename_map(functions, unit_tag);
    std::string preamble =
        generate_preamble(source, functions, unit_tag, abs_path, static_renames);

    {
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
    }

    emit_split_files(tu, functions, input_path, abs_path, unit_dir,
                     split_include_root(output_dir), unit_tag,
                     preamble_filename, preamble_path, all_flags, extra_flags,
                     input_is_header, parse_errors == 0, verbose, out, result);

    result.success = true;
    return result;
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
        std::string rel = header_mirror_relpath(abs_path, include_dirs_from_flags(extra_flags));
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

    std::vector<std::string> parse_flags_vec = all_flags;
    std::string libclang_pch = build_libclang_pch(preamble_path, all_flags, verbose, out);
    if (!libclang_pch.empty()) {
        parse_flags_vec.push_back("-include-pch");
        parse_flags_vec.push_back(libclang_pch);
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
    unsigned parse_flags = 0;
    if (libclang_pch.empty())
        parse_flags = CXTranslationUnit_PrecompiledPreamble
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

    std::vector<FunctionInfo> functions;
    VisitorData vd{tu, &source, &functions, &abs_path};
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, &vd);
    prepare_functions(functions);

    if (functions.empty()) {
        if (verbose) out << "No function definitions found in " << input_path << "\n";
        if (!input_is_header) {
            resolve_header_deps(tu, result, output_dir, extra_flags, verbose, out);
        }
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        result.success = true;
        return result;
    }

    if (!claim_output_path(preamble_path, abs_path))
        return result;

    fs::create_directories(unit_dir);

    result.preamble_filename = preamble_path;
    const StaticRenameMap static_renames = build_static_rename_map(functions, unit_tag);
    std::string preamble =
        generate_preamble(source, functions, unit_tag, abs_path, static_renames);

    {
        std::string existing_preamble;
        if (fs::exists(preamble_path))
            existing_preamble = read_file(preamble_path);
        if (existing_preamble != preamble) {
            std::ofstream ofs(preamble_path);
            if (!ofs.is_open()) {
                std::cerr << "Error: cannot write preamble to " << preamble_path << "\n";
                clang_disposeTranslationUnit(tu);
                clang_disposeIndex(index);
                return result;
            }
            ofs << preamble;
            ofs.close();
            if (verbose) out << "Generated preamble: " << preamble_path << " (updated)\n";
        } else {
            if (verbose) out << "Preamble unchanged: " << preamble_path << "\n";
        }
    }

    emit_split_files(tu, functions, input_path, abs_path, unit_dir,
                     split_include_root(output_dir), unit_tag,
                     preamble_filename, preamble_path, all_flags, extra_flags,
                     input_is_header, parse_errors == 0, verbose, out, result);

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);

    result.success = true;
    return result;
}

static bool launcher_verbose() {
    const char* val = std::getenv("CPP_SPLITTER_VERBOSE");
    if (val && (std::string(val) == "1" || std::string(val) == "on")) return true;
    return false;
}

static int run_as_launcher(int argc, char* argv[]) {
    bool verbose = launcher_verbose();
    std::string compiler = argv[1];

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
    
    SplitResult sr;
    {
        const char* no_server = std::getenv("CPP_SPLITTER_NO_SERVER");
        if (!no_server || std::string(no_server) != "1") {
            sr = try_server_split(input_file, split_dir, split_flags, verbose, std::cout);
        } else {
          std::cerr << "[cpp-splitter] server unavailable, splitting locally\n";
          sr = do_split(input_file, split_dir, split_flags, verbose);
        }
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
        if (verbose) std::cerr << "[cpp-splitter] splitting failed, falling back to normal compilation\n";
        std::string cmd = build_passthrough_cmd();
        if (verbose) std::cerr << "[cpp-splitter] passthrough: " << cmd << "\n";
        return run_command_quiet(cmd);
    }

    if (sr.compilable_files.empty()) {
        std::string cmd = build_passthrough_cmd();
        if (verbose) std::cerr << "[cpp-splitter] no compilable files, passthrough: " << cmd << "\n";
        return run_command_quiet(cmd);
    }

    if (compiler == "tipi-compiler-driver") {
      std::cout << "BEGIN compiler_with_driver is: " << std::endl;
      auto pch_flags = other_flags;
      auto actual_compiler = *pch_flags.begin();
      pch_flags = std::vector<std::string>(pch_flags.begin()+1, pch_flags.end());
      std::cout << "compiler_with_driver is: " << compiler << " " << actual_compiler<< std::endl;
      build_pch(sr.preamble_filename, actual_compiler, "", pch_flags, split_dir, verbose, std::cerr);
    } else {
      build_pch(sr.preamble_filename, compiler, "", other_flags, split_dir, verbose, std::cerr);
    }

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

        for (const auto& f : other_flags)
            cmd += " " + shell_quote(f);

        cmd += " -I" + shell_quote(split_dir);
        cmd += " -I" + shell_quote(split_include_root(split_dir));
        for (const auto& hdr_dir : sr.header_obj_dirs)
            cmd += " -I" + shell_quote(hdr_dir);

        if (fi == 0 && (has_md || has_mmd)) {
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
                for (const auto& f : other_flags)
                    cmd += " " + shell_quote(f);
                for (const auto& hdr_dir : sr.header_obj_dirs)
                    cmd += " -I" + shell_quote(hdr_dir);
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
        bool need_link = (launcher_skipped < (int)sr.compilable_files.size()) || !sr.header_obj_files.empty() || !fs::exists(output_file);

        if (!need_link) {
            if (verbose) std::cerr << "[cpp-splitter] all up-to-date, skipping link: " << output_file << "\n";
        } else if (obj_files.size() == 1) {
            if (verbose) std::cerr << "[cpp-splitter] single .o, copying " << obj_files[0] << " -> " << output_file << "\n";
            fs::copy_file(obj_files[0], output_file, fs::copy_options::overwrite_existing);
        } else {
            std::string cmd = "ld -r -o " + shell_quote(output_file);
            for (const auto& obj : obj_files)
                cmd += " " + shell_quote(obj);
            if (verbose) std::cerr << "[cpp-splitter] ld -r: " << cmd << "\n";
            int ret = run_command_quiet(cmd);
            if (ret != 0) {
                std::cerr << "cpp-splitter: relocatable link failed\n";
                split_build_failed = true;
            }
        }
    }

    if (split_build_failed) {
        std::cerr << "[cpp-splitter] split build failed, falling back to normal compilation\n";
        std::string cmd = build_passthrough_cmd();
        if (verbose) std::cerr << "[cpp-splitter] passthrough: " << cmd << "\n";
        return run_command_quiet(cmd);
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
              << "  compiler wrapper. Splits the source via the server, compiles\n"
              << "  each piece, and combines them into a single .o via relocatable\n"
              << "  linking. Requires the server to be running.\n"
              << "\n"
              << "  Non-compilation commands are passed through transparently.\n"
              << "\n"
              << "  CMake usage:\n"
              << "    " << prog << " --server &\n"
              << "    cmake -DCMAKE_CXX_COMPILER_LAUNCHER=/path/to/" << prog << " ..\n"
              << "\n"
              << "Mode 3 - Server (persistent TU cache):\n"
              << "  Starts a background server that keeps parsed translation units\n"
              << "  in memory for faster re-splitting on subsequent invocations.\n"
              << "\n"
              << "  " << prog << " --server [--socket <path>]\n"
              << "\n"
              << "  The server listens on a Unix domain socket (default:\n"
              << "  /tmp/cpp-splitter-<uid>.sock). Clients connect automatically.\n"
              << "  Set CPP_SPLITTER_SOCKET to override the socket path.\n"
              << "  Set CPP_SPLITTER_NO_SERVER=1 to disable client connections.\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << " src/app.cpp output                          # split only\n"
              << "  " << prog << " src/app.cpp output --compile -o myapp       # split + compile + link\n"
              << "  " << prog << " g++ -std=c++17 -c -o foo.o foo.cpp          # launcher mode\n"
              << "  " << prog << " --server                                    # start TU cache server\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string first_arg = argv[1];

    if (first_arg == "--server") {
        std::string sock_path = default_socket_path();
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--socket" && i + 1 < argc) {
                sock_path = argv[++i];
            } else {
                std::cerr << "Unknown server option: " << arg << "\n";
                return 1;
            }
        }
        return run_server(sock_path);
    }

    if (!first_arg.empty() && first_arg[0] != '-' && !is_splittable_file(first_arg)) {
        return run_as_launcher(argc, argv);
    }

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

    SplitResult sr;
    {
        const char* no_server = std::getenv("CPP_SPLITTER_NO_SERVER");
        if (!no_server || std::string(no_server) != "1") {
            sr = try_server_split(input_path, output_dir, extra_flags, true, std::cout);
        }
        if (!sr.success) {
            sr = do_split(input_path, output_dir, extra_flags, true);
        }
    }

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

        std::vector<std::string> pch_flags = {"-std=c++17"};
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

                std::string cmd = cxx_compiler + " -std=c++17 -c";
                for (const auto& hdr_dir : sr.header_obj_dirs)
                    cmd += " -I" + hdr_dir;
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
