#include <clang-c/Index.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/process.hpp>

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

static std::string generate_forward_decl(const FunctionInfo& fn,
                                          const std::string& stem) {
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

    if (fn.is_static) {
        std::string mangled_qname = fn.qualified_name;
        size_t npos = mangled_qname.find(fn.name);
        if (npos != std::string::npos)
            mangled_qname.replace(npos, fn.name.size(),
                                  make_static_mangled_name(stem, fn.name));
        decl += mangled_qname + ";";
    } else {
        decl += fn.qualified_name + ";";
    }

    for (size_t i = 0; i < ns_names.size(); ++i)
        decl += " }";

    return decl;
}

static std::string generate_preamble(const std::string& source,
                                     const std::vector<FunctionInfo>& functions,
                                     const std::string& stem,
                                     const std::string& source_path) {
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

    auto line_offsets = build_line_offsets(source);

    std::string preamble;
    preamble += "#pragma once\n";
    preamble += "#line 1 \"" + source_path + "\"\n";

    auto ensure_newline = [&preamble]() {
        if (!preamble.empty() && preamble.back() != '\n')
            preamble += '\n';
    };

    unsigned pos = 0;
    for (const auto& r : ranges) {
        if (r.start > pos) {
            preamble += source.substr(pos, r.start - pos);
        }
        if (r.keep) {
            ensure_newline();
            unsigned keep_line = offset_to_line(line_offsets, r.start);
            preamble += "#line " + std::to_string(keep_line) + " \"" + source_path + "\"\n";
            preamble += source.substr(r.start, r.end - r.start);
        }
        ensure_newline();
        unsigned resume_line = offset_to_line(line_offsets, r.end);
        preamble += "#line " + std::to_string(resume_line) + " \"" + source_path + "\"\n";
        pos = r.end;
    }
    if (pos < source.size()) {
        preamble += source.substr(pos);
    }

    preamble += "\n";
    for (const auto& fn : functions) {
        if (should_keep_in_header(fn))
            continue;
        std::string decl = generate_forward_decl(fn, stem);
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
                                                    bool verbose) {
    std::vector<CompileResult> results(jobs.size());
    std::mutex output_mtx;
    std::vector<std::thread> threads;
    std::atomic<size_t> next_job{0};

    unsigned num_threads = std::min(static_cast<unsigned>(jobs.size()), get_parallelism());

    if (verbose) {
        std::lock_guard<std::mutex> lock(output_mtx);
        std::cout << "  [parallel: " << num_threads << " threads, "
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
                    std::cout << "  $ " << job.cmd << "\n";
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

static bool is_source_file(const std::string& path) {
    static const char* exts[] = {".cpp", ".cc", ".cxx", ".C", ".c++", ".cp", ".c"};
    for (const char* ext : exts) {
        size_t elen = std::strlen(ext);
        if (path.size() >= elen && path.compare(path.size() - elen, elen, ext) == 0)
            return true;
    }
    return false;
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

struct SplitResult {
    std::vector<std::string> compilable_files;
    std::string preamble_filename;
    bool success;
};

static SplitResult do_split(const std::string& input_path,
                            const std::string& output_dir,
                            const std::vector<std::string>& extra_flags,
                            bool verbose) {
    SplitResult result;
    result.success = false;

    std::string abs_path = fs::absolute(input_path).string();
    std::string source = read_file(abs_path);
    if (source.empty()) {
        if (verbose) std::cerr << "Error: could not read file or file is empty\n";
        return result;
    }

    std::string stem = fs::path(input_path).stem().string();

    CXIndex index = clang_createIndex(0, 0);
    if (!index) {
        if (verbose) std::cerr << "Error: failed to create clang index\n";
        return result;
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
        if (verbose) std::cerr << "Error: failed to parse translation unit (code: " << err << ")\n";
        clang_disposeIndex(index);
        return result;
    }

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

    if (error_count > 0 && verbose) {
        std::cerr << "Warning: " << error_count << " parse error(s) found. "
                  << "Output may be incomplete.\n";
    }

    std::vector<FunctionInfo> functions;
    VisitorData vd{tu, &source, &functions, &abs_path};

    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, &vd);

    if (functions.empty()) {
        if (verbose) std::cout << "No function definitions found in " << input_path << "\n";
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        result.success = true;
        return result;
    }

    fs::create_directories(output_dir);

    std::string preamble_filename = stem + "_preamble.h";
    result.preamble_filename = preamble_filename;
    std::string preamble_path = (fs::path(output_dir) / preamble_filename).string();
    std::string preamble = generate_preamble(source, functions, stem, abs_path);

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
            if (verbose) std::cout << "Generated preamble: " << preamble_path << " (updated)\n";
        } else {
            if (verbose) std::cout << "Preamble unchanged: " << preamble_path << "\n";
        }
    }

    if (verbose) std::cout << "Found " << functions.size() << " function(s) in " << input_path << ":\n\n";

    std::vector<std::pair<std::string, std::string>> static_renames;
    for (const auto& fn : functions) {
        if (fn.is_static) {
            static_renames.emplace_back(fn.name,
                                        make_static_mangled_name(stem, fn.name));
        }
    }

    auto apply_static_renames = [&](std::string text) -> std::string {
        for (const auto& [orig, mangled] : static_renames) {
            size_t pos = 0;
            while ((pos = text.find(orig, pos)) != std::string::npos) {
                if (pos > 0 && (std::isalnum(static_cast<unsigned char>(text[pos - 1])) || text[pos - 1] == '_')) {
                    pos += orig.size();
                    continue;
                }
                size_t end = pos + orig.size();
                if (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
                    pos += orig.size();
                    continue;
                }
                text.replace(pos, orig.size(), mangled);
                pos += mangled.size();
            }
        }
        return text;
    };

    std::vector<std::string> current_files;
    int file_counter = 0;
    int written_count = 0;
    int skipped_count = 0;
    for (const auto& fn : functions) {
        ++file_counter;

        std::string safe_name = sanitize_filename(fn.name);
        if (safe_name.empty()) safe_name = "anonymous";

        std::string out_filename = stem + "_" +
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
        body = apply_static_renames(body);

        std::string line_directive = "#line " + std::to_string(fn.start_line) +
                                     " \"" + abs_path + "\"\n";
        body = line_directive + body;

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
            std::cout << "  [" << file_counter << "] " << fn.signature;
            if (kept) std::cout << "  (header-only)";
            if (!needs_write) std::cout << "  (unchanged)";
            std::cout << "\n";
            std::cout << "      Lines " << fn.start_line << "-" << fn.end_line
                      << " -> " << out_path << "\n";
        }
    }

    int removed_count = 0;
    for (const auto& entry : fs::directory_iterator(output_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string path = entry.path().string();
        std::string fname = entry.path().filename().string();
        if (fname == preamble_filename) continue;
        if (fname.size() < 4 || fname.substr(fname.size() - 4) != ".cpp") continue;
        if (fname.rfind(stem + "_", 0) != 0) continue;
        if (std::find(current_files.begin(), current_files.end(), path) == current_files.end()) {
            fs::remove(entry.path());
            fs::path obj_path = entry.path();
            obj_path.replace_extension(".o");
            if (fs::exists(obj_path))
                fs::remove(obj_path);
            if (verbose) std::cout << "  Removed stale: " << fname << "\n";
            ++removed_count;
        }
    }

    if (verbose) {
        std::cout << "\n" << file_counter << " function(s): "
                  << written_count << " written, "
                  << skipped_count << " unchanged";
        if (removed_count > 0)
            std::cout << ", " << removed_count << " stale removed";
        std::cout << "\n";
    }

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);

    result.success = true;
    return result;
}

static int run_as_launcher(int argc, char* argv[]) {
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
        return run_command_quiet(cmd);
    }

    if (!fs::exists(input_file)) {
        std::cerr << "cpp-splitter: source file not found: " << input_file << "\n";
        return 1;
    }

    std::string split_dir;
    if (!output_file.empty()) {
        split_dir = output_file + ".split";
    } else {
        split_dir = fs::path(input_file).stem().string() + ".split";
    }

    auto sr = do_split(input_file, split_dir, other_flags, false);

    if (!sr.success || sr.compilable_files.empty()) {
        std::string cmd;
        for (int i = 1; i < argc; i++) {
            if (i > 1) cmd += " ";
            cmd += shell_quote(argv[i]);
        }
        return run_command_quiet(cmd);
    }

    std::vector<std::string> obj_files;
    std::vector<CompileJob> parallel_jobs;

    for (size_t fi = 0; fi < sr.compilable_files.size(); ++fi) {
        const auto& cpp = sr.compilable_files[fi];
        std::string obj = cpp.substr(0, cpp.size() - 4) + ".o";

        std::string cmd = shell_quote(compiler);

        for (const auto& f : other_flags)
            cmd += " " + shell_quote(f);

        cmd += " -I" + shell_quote(split_dir);

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
            int ret = run_command_quiet(cmd);
            if (ret != 0) {
                std::cerr << "cpp-splitter: compilation failed for split file: " << cpp << "\n";
                return ret;
            }
        } else {
            parallel_jobs.push_back({cmd, cpp, obj});
        }

        obj_files.push_back(obj);
    }

    if (!parallel_jobs.empty()) {
        auto results = compile_parallel(parallel_jobs, false);
        for (const auto& r : results) {
            if (r.exit_code != 0) {
                std::cerr << "cpp-splitter: compilation failed for split file: " << r.source_file << "\n";
                if (!r.stderr_output.empty()) std::cerr << r.stderr_output;
                return 1;
            }
        }
    }

    if (output_file.empty())
        output_file = fs::path(input_file).stem().string() + ".o";

    if (obj_files.size() == 1) {
        fs::copy_file(obj_files[0], output_file, fs::copy_options::overwrite_existing);
    } else {
        std::string cmd = "ld -r -o " + shell_quote(output_file);
        for (const auto& obj : obj_files)
            cmd += " " + shell_quote(obj);
        int ret = run_command_quiet(cmd);
        if (ret != 0) {
            std::cerr << "cpp-splitter: relocatable link failed\n";
            return ret;
        }
    }

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
              << "  compiler wrapper. Splits the source, compiles each piece,\n"
              << "  and combines them into a single .o via relocatable linking.\n"
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
    if (!first_arg.empty() && first_arg[0] != '-' && !is_source_file(first_arg)) {
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

    auto sr = do_split(input_path, output_dir, extra_flags, true);
    if (!sr.success) return 1;
    if (sr.compilable_files.empty()) return 0;

    if (do_compile) {
        std::cout << "\n--- Compiling split files ---\n\n";

        std::vector<CompileJob> jobs;
        std::vector<std::string> obj_files;

        for (const auto& cpp_file : sr.compilable_files) {
            std::string obj_file = cpp_file.substr(0, cpp_file.size() - 4) + ".o";
            std::string cmd = cxx_compiler + " -std=c++17 -c"
                              " -I" + output_dir +
                              " -o " + obj_file +
                              " " + cpp_file;

            for (const auto& f : extra_flags)
                cmd += " " + f;

            jobs.push_back({cmd, cpp_file, obj_file});
            obj_files.push_back(obj_file);
        }

        auto results = compile_parallel(jobs, true);
        bool compile_ok = true;
        for (const auto& r : results) {
            if (r.exit_code != 0) {
                std::cerr << "Error: compilation failed for " << r.source_file << "\n";
                if (!r.stderr_output.empty()) std::cerr << r.stderr_output;
                compile_ok = false;
            }
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
