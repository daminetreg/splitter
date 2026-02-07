#include <clang-c/Index.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

    CXString qualified = clang_getCursorDisplayName(cursor);
    info.qualified_name = cx_to_string(qualified);

    CXType ret_type = clang_getCursorResultType(cursor);
    info.return_type = cx_to_string(clang_getTypeSpelling(ret_type));

    std::string class_prefix;
    CXCursor parent_cursor = clang_getCursorSemanticParent(cursor);
    std::vector<ScopeEntry> scope_parts;
    while (true) {
        CXCursorKind pk = clang_getCursorKind(parent_cursor);
        if (pk == CXCursor_ClassDecl || pk == CXCursor_StructDecl ||
            pk == CXCursor_ClassTemplate || pk == CXCursor_Namespace) {
            std::string pname = cx_to_string(clang_getCursorSpelling(parent_cursor));
            ScopeKind sk = (pk == CXCursor_Namespace) ? ScopeKind::Namespace : ScopeKind::Class;
            if (!pname.empty())
                scope_parts.push_back({pname, sk});
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

static bool should_keep_in_header(const FunctionInfo& fn) {
    return fn.is_template;
}

static std::string make_static_mangled_name(const std::string& stem, const std::string& name) {
    std::string safe_stem;
    for (char c : stem) {
        safe_stem += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return "__static_" + safe_stem + "__" + name;
}

static std::string generate_forward_decl(const FunctionInfo& fn) {
    bool is_class_method = false;
    for (const auto& entry : fn.scope_chain) {
        if (entry.kind == ScopeKind::Class) {
            is_class_method = true;
            break;
        }
    }
    if (is_class_method)
        return "";

    std::string decl;
    std::vector<std::string> ns_names;
    for (const auto& entry : fn.scope_chain) {
        if (entry.kind == ScopeKind::Namespace)
            ns_names.push_back(entry.name);
    }

    for (const auto& ns : ns_names)
        decl += "namespace " + ns + " { ";

    if (!fn.return_type.empty())
        decl += fn.return_type + " ";
    decl += fn.qualified_name + ";";

    for (size_t i = 0; i < ns_names.size(); ++i)
        decl += " }";

    return decl;
}

static std::string generate_preamble(const std::string& source,
                                     const std::vector<FunctionInfo>& functions,
                                     const std::string& stem) {
    struct Range {
        unsigned start, end;
        bool keep;
    };
    std::vector<Range> ranges;
    for (const auto& fn : functions) {
        ranges.push_back({fn.start_offset, fn.end_offset, should_keep_in_header(fn)});
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const Range& a, const Range& b) { return a.start < b.start; });

    std::string preamble;
    preamble += "#pragma once\n";

    unsigned pos = 0;
    for (const auto& r : ranges) {
        if (r.start > pos) {
            preamble += source.substr(pos, r.start - pos);
        }
        if (r.keep) {
            preamble += source.substr(r.start, r.end - r.start);
        }
        pos = r.end;
    }
    if (pos < source.size()) {
        preamble += source.substr(pos);
    }

    preamble += "\n";
    for (const auto& fn : functions) {
        if (should_keep_in_header(fn))
            continue;
        if (fn.is_static) {
            std::string mangled = make_static_mangled_name(stem, fn.name);
            preamble += "#define " + fn.name + " " + mangled + "\n";
        }
    }
    preamble += "\n";
    for (const auto& fn : functions) {
        if (should_keep_in_header(fn))
            continue;
        std::string decl = generate_forward_decl(fn);
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

static int run_command(const std::string& cmd) {
    std::cout << "  $ " << cmd << "\n";
    int ret = std::system(cmd.c_str());
    return WEXITSTATUS(ret);
}

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <input.cpp> [output_dir] [options] [-- <clang_flags>...]\n"
              << "\n"
              << "Parses the input .cpp file using libclang AST and generates\n"
              << "one file per function/method implementation.\n"
              << "\n"
              << "Arguments:\n"
              << "  input.cpp      The C++ source file to split\n"
              << "  output_dir     Directory for output files (default: ./output)\n"
              << "\n"
              << "Options:\n"
              << "  --compile      Compile and link the split files into a binary\n"
              << "  -o <binary>    Output binary name (default: <stem>.out)\n"
              << "  --cxx <comp>   C++ compiler to use (default: g++)\n"
              << "  -- <flags>     Extra flags passed to clang parser\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << " src/app.cpp output\n"
              << "  " << prog << " src/app.cpp output --compile -o myapp\n"
              << "  " << prog << " src/app.cpp output --compile -- -I/usr/include\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
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

    std::string abs_path = fs::absolute(input_path).string();
    std::string source = read_file(abs_path);
    if (source.empty()) {
        std::cerr << "Error: could not read file or file is empty\n";
        return 1;
    }

    std::string stem = fs::path(input_path).stem().string();
    if (output_binary.empty())
        output_binary = stem + ".out";

    CXIndex index = clang_createIndex(0, 0);
    if (!index) {
        std::cerr << "Error: failed to create clang index\n";
        return 1;
    }

    std::vector<std::string> all_flags = {"-std=c++17", "-fsyntax-only", "-Wno-everything"};
    for (const auto& f : extra_flags)
        all_flags.push_back(f);

    std::vector<const char*> args;
    for (const auto& f : all_flags)
        args.push_back(f.c_str());

    CXTranslationUnit tu = nullptr;
    CXErrorCode err = clang_parseTranslationUnit2(
        index,
        abs_path.c_str(),
        args.data(),
        static_cast<int>(args.size()),
        nullptr, 0,
        CXTranslationUnit_None,
        &tu);

    if (err != CXError_Success || !tu) {
        std::cerr << "Error: failed to parse translation unit (code: " << err << ")\n";
        clang_disposeIndex(index);
        return 1;
    }

    unsigned num_diag = clang_getNumDiagnostics(tu);
    unsigned error_count = 0;
    for (unsigned i = 0; i < num_diag; ++i) {
        CXDiagnostic diag = clang_getDiagnostic(tu, i);
        CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diag);
        if (sev >= CXDiagnostic_Error) {
            std::cerr << "Parse error: "
                      << cx_to_string(clang_formatDiagnostic(diag, CXDiagnostic_DisplaySourceLocation))
                      << "\n";
            ++error_count;
        }
        clang_disposeDiagnostic(diag);
    }

    if (error_count > 0) {
        std::cerr << "Warning: " << error_count << " parse error(s) found. "
                  << "Output may be incomplete.\n";
    }

    std::vector<FunctionInfo> functions;
    VisitorData vd{tu, &source, &functions, &abs_path};

    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, &vd);

    if (functions.empty()) {
        std::cout << "No function definitions found in " << input_path << "\n";
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return 0;
    }

    fs::create_directories(output_dir);

    std::string preamble_filename = stem + "_preamble.h";
    std::string preamble_path = (fs::path(output_dir) / preamble_filename).string();
    std::string preamble = generate_preamble(source, functions, stem);

    {
        std::ofstream ofs(preamble_path);
        if (!ofs.is_open()) {
            std::cerr << "Error: cannot write preamble to " << preamble_path << "\n";
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return 1;
        }
        ofs << preamble;
        ofs.close();
        std::cout << "Generated preamble: " << preamble_path << "\n";
    }

    std::cout << "Found " << functions.size() << " function(s) in " << input_path << ":\n\n";

    std::vector<std::string> compilable_files;
    int file_counter = 0;
    for (const auto& fn : functions) {
        ++file_counter;

        std::string safe_name = sanitize_filename(fn.name);
        if (safe_name.empty()) safe_name = "anonymous";

        std::string out_filename = stem + "_" +
                                   std::to_string(file_counter) + "_" +
                                   safe_name + ".cpp";
        std::string out_path = (fs::path(output_dir) / out_filename).string();

        std::ofstream ofs(out_path);
        if (!ofs.is_open()) {
            std::cerr << "Error: cannot write to " << out_path << "\n";
            continue;
        }

        ofs << "// Function: " << fn.signature << "\n";
        ofs << "// Source: " << input_path << " (lines " << fn.start_line << "-" << fn.end_line << ")\n";
        if (should_keep_in_header(fn))
            ofs << "// Note: template - kept in preamble header for compilation\n";
        ofs << "// ---\n\n";
        ofs << "#include \"" << preamble_filename << "\"\n\n";

        std::string body = fn.body;
        if (fn.is_static) {
            size_t spos = body.find("static");
            if (spos != std::string::npos) {
                size_t after = spos + 6;
                while (after < body.size() && body[after] == ' ')
                    ++after;
                body.erase(spos, after - spos);
            }
        }

        if (!fn.scope_chain.empty()) {
            ofs << wrap_in_namespaces(body, fn.scope_chain) << "\n";
        } else {
            ofs << body << "\n";
        }

        ofs.close();

        bool kept = should_keep_in_header(fn);
        if (!kept)
            compilable_files.push_back(out_path);

        std::cout << "  [" << file_counter << "] " << fn.signature;
        if (kept) std::cout << "  (header-only)";
        std::cout << "\n";
        std::cout << "      Lines " << fn.start_line << "-" << fn.end_line
                  << " -> " << out_path << "\n";
    }

    std::cout << "\nWrote " << file_counter << " file(s) to " << output_dir << "/\n";

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);

    if (do_compile) {
        std::cout << "\n--- Compiling split files ---\n\n";

        std::vector<std::string> obj_files;
        bool compile_ok = true;

        for (const auto& cpp_file : compilable_files) {
            std::string obj_file = cpp_file.substr(0, cpp_file.size() - 4) + ".o";
            std::string cmd = cxx_compiler + " -std=c++17 -c"
                              " -I" + output_dir +
                              " -o " + obj_file +
                              " " + cpp_file;

            for (const auto& f : extra_flags)
                cmd += " " + f;

            int ret = run_command(cmd);
            if (ret != 0) {
                std::cerr << "Error: compilation failed for " << cpp_file << "\n";
                compile_ok = false;
                break;
            }
            obj_files.push_back(obj_file);
        }

        if (compile_ok) {
            std::cout << "\n--- Linking ---\n\n";

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
            return 1;
        }
    }

    return 0;
}
