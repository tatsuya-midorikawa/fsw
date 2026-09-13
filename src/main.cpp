#include "fsw.hpp"

#include <chrono>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;

void usage() {
    std::cout <<
        "fsw 0.1.0-dev - FSharp to Wasm\n"
        "Usage: fsw [build|check] source.fs [-o output.wasm] [options]\n"
        "  -o, --output PATH   Output path (default: source.wasm)\n"
        "  --export NAME       Export only selected declarations; repeatable\n"
        "  --no-opt            Disable constant folding and tail-call elimination\n"
        "  --stats             Write compilation statistics as JSON to stderr\n"
        "  --max-memory-pages N  Linear memory ceiling in 64 KiB pages (default: 256)\n"
        "  --version           Show compiler version\n"
        "  -h, --help          Show this help\n";
}

std::string read_source(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) throw std::runtime_error("cannot read '" + path.u8string() + "': " + error.message());
    if (size > fsw::max_source_bytes) throw std::runtime_error("source exceeds the 16 MiB limit");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open '" + path.u8string() + "'");
    std::string source(static_cast<std::size_t>(size), '\0');
    input.read(source.data(), static_cast<std::streamsize>(source.size()));
    if (!input || input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("failed to read source or source changed while reading");
    return source;
}

void write_output(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    const auto parent = path.has_parent_path() ? path.parent_path() : fs::path(".");
    std::random_device random;
    fs::path directory;
    for (int attempt = 0; attempt < 16; ++attempt) {
        directory = parent / (".fsw-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::error_code error;
        if (fs::create_directory(directory, error)) break;
        if (error && error != std::errc::file_exists)
            throw std::runtime_error("cannot create output temporary directory: " + error.message());
        directory.clear();
    }
    if (directory.empty()) throw std::runtime_error("cannot create a unique output temporary directory");
    const auto temporary = directory / "module.wasm";
    struct Cleanup {
        fs::path file, directory;
        ~Cleanup() {
            for (const auto& path : {file, directory}) {
                std::error_code error;
                fs::remove(path, error);
                if (error) std::cerr << "fsw: failed to clean '" << path.u8string() << "': " << error.message() << '\n';
            }
        }
    } cleanup{temporary, directory};
    std::ofstream output(temporary, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open temporary output");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("failed to write output");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "cannot replace output");
#else
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) throw std::runtime_error("cannot replace output: " + error.message());
#endif
}

int run(const std::vector<std::string>& args) {
    fs::path source_path;
    try {
        if (args.empty()) { usage(); return 2; }
        fsw::Options options;
        fs::path output_path;
        bool checking = false, stats = false, positional = false;
        std::size_t index = 0;
        if (args[0] == "build" || args[0] == "check") { checking = args[0] == "check"; ++index; }
        for (; index < args.size(); ++index) {
            const auto& arg = args[index];
            if (!positional && arg == "--") { positional = true; continue; }
            if (!positional && (arg == "--help" || arg == "-h")) { usage(); return 0; }
            if (!positional && arg == "--version") { std::cout << "fsw 0.1.0-dev\n"; return 0; }
            if (!positional && arg == "--stats") { stats = true; continue; }
            if (!positional && arg == "--no-opt") { options.optimize = false; continue; }
            if (!positional && (arg == "-o" || arg == "--output" || arg == "--export" || arg == "--max-memory-pages")) {
                if (++index == args.size()) throw std::runtime_error("missing value for " + arg);
                if (arg == "--max-memory-pages") {
                    const auto& value = args[index];
                    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.max_memory_pages);
                    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                        !options.max_memory_pages || options.max_memory_pages > 32767)
                        throw std::runtime_error("maximum memory must be between 1 and 32767 pages");
                } else if (arg == "--export") options.exports.push_back(args[index]);
                else {
                    if (!output_path.empty()) throw std::runtime_error("output path specified more than once");
                    output_path = fs::u8path(args[index]);
                }
                continue;
            }
            if (!positional && !arg.empty() && arg[0] == '-') throw std::runtime_error("unknown option '" + arg + "'");
            if (!source_path.empty()) throw std::runtime_error("expected exactly one source file");
            source_path = fs::u8path(arg);
        }
        if (source_path.empty()) throw std::runtime_error("no source file specified");
        if (checking && !output_path.empty()) throw std::runtime_error("'check' does not accept an output path");
        if (output_path.empty()) { output_path = source_path; output_path.replace_extension(".wasm"); }
        if (!checking) {
            std::error_code error;
            if (fs::absolute(source_path).lexically_normal() == fs::absolute(output_path).lexically_normal() ||
                fs::equivalent(source_path, output_path, error))
                throw std::runtime_error("output must not overwrite the source file");
        }
        const auto source = read_source(source_path);
        const auto start = std::chrono::steady_clock::now();
        const auto output = fsw::compile(source, options);
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!checking) write_output(output_path, output.bytes);
        if (stats) {
            std::cerr << std::fixed << std::setprecision(3)
                << "{\"source_bytes\":" << source.size() << ",\"wasm_bytes\":" << output.bytes.size()
                << ",\"functions\":" << output.functions << ",\"compile_ms\":" << elapsed << "}\n";
        }
        return 0;
    } catch (const fsw::Error& error) {
        std::cerr << source_path.u8string() << ':' << error.position.line << ':' << error.position.column
                  << ": error: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "fsw: " << error.what() << '\n';
        return 1;
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!size) { std::cerr << "fsw: invalid command-line encoding\n"; return 1; }
        std::string value(static_cast<std::size_t>(size), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), size, nullptr, nullptr)) {
            std::cerr << "fsw: cannot decode command-line argument\n"; return 1;
        }
        value.pop_back();
        args.push_back(std::move(value));
    }
    return run(args);
}
#else
int main(int argc, char** argv) { return run({argv + 1, argv + argc}); }
#endif
