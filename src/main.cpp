#include <clang-c/Index.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct FunctionInfo {
    std::string name;
    std::string qualified_name;
    std::string return_type;
    std::string signature;
    std::string body;
    unsigned start_line;
    unsigned end_line;
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

    CXString qualified = clang_getCursorDisplayName(cursor);
    info.qualified_name = cx_to_string(qualified);

    CXType ret_type = clang_getCursorResultType(cursor);
    info.return_type = cx_to_string(clang_getTypeSpelling(ret_type));

    std::string class_prefix;
    CXCursor parent_cursor = clang_getCursorSemanticParent(cursor);
    std::vector<std::string> scope_parts;
    while (true) {
        CXCursorKind pk = clang_getCursorKind(parent_cursor);
        if (pk == CXCursor_ClassDecl || pk == CXCursor_StructDecl ||
            pk == CXCursor_ClassTemplate || pk == CXCursor_Namespace) {
            std::string pname = cx_to_string(clang_getCursorSpelling(parent_cursor));
            if (!pname.empty())
                scope_parts.push_back(pname);
            parent_cursor = clang_getCursorSemanticParent(parent_cursor);
        } else {
            break;
        }
    }
    for (auto it = scope_parts.rbegin(); it != scope_parts.rend(); ++it)
        class_prefix += *it + "::";

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

    unsigned start_line, end_line;
    clang_getFileLocation(start_loc, nullptr, &start_line, nullptr, nullptr);
    clang_getFileLocation(end_loc, nullptr, &end_line, nullptr, nullptr);
    info.start_line = start_line;
    info.end_line = end_line;

    info.body = get_source_range_text(vd->tu, extent);

    vd->functions->push_back(std::move(info));

    return CXChildVisit_Continue;
}

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <input.cpp> [output_dir] [-- <clang_flags>...]\n"
              << "\n"
              << "Parses the input .cpp file using libclang AST and generates\n"
              << "one file per function/method implementation.\n"
              << "\n"
              << "Arguments:\n"
              << "  input.cpp      The C++ source file to split\n"
              << "  output_dir     Directory for output files (default: ./output)\n"
              << "  -- <flags>     Extra flags passed to clang (e.g. -I/path/to/include)\n"
              << "\n"
              << "Example:\n"
              << "  " << prog << " src/app.cpp output -- -I/usr/include -std=c++20\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string input_path = argv[1];
    std::string output_dir = "output";
    std::vector<std::string> extra_flags;

    int i = 2;
    if (i < argc && std::string(argv[i]) != "--") {
        output_dir = argv[i];
        ++i;
    }
    if (i < argc && std::string(argv[i]) == "--") {
        ++i;
        for (; i < argc; ++i)
            extra_flags.emplace_back(argv[i]);
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

    std::string stem = fs::path(input_path).stem().string();

    std::cout << "Found " << functions.size() << " function(s) in " << input_path << ":\n\n";

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
        ofs << "// ---\n\n";
        ofs << fn.body << "\n";
        ofs.close();

        std::cout << "  [" << file_counter << "] " << fn.signature << "\n";
        std::cout << "      Lines " << fn.start_line << "-" << fn.end_line
                  << " -> " << out_path << "\n";
    }

    std::cout << "\nWrote " << file_counter << " file(s) to " << output_dir << "/\n";

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 0;
}
