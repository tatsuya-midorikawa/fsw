#include "../src/fsw.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

#ifdef FSW_STANDALONE_FUZZ
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 256 * 1024) return 0;
    try {
        fsw::compile(std::string_view(reinterpret_cast<const char*>(data), size));
    } catch (const fsw::Error&) {
        // Invalid source must produce a diagnostic, not an internal exception or a crash.
    }
    return 0;
}

#ifdef FSW_STANDALONE_FUZZ
int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: fsw-fuzz seed.fs [...]\n"; return 2; }
    std::vector<std::string> seeds;
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary);
        if (!input) { std::cerr << "cannot open fuzz seed: " << argv[i] << '\n'; return 1; }
        seeds.emplace_back(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    std::mt19937 random(0x465357);
    for (int i = 0; i < 20000; ++i) {
        auto source = seeds[random() % seeds.size()];
        const unsigned mutations = 1 + random() % 8;
        for (unsigned n = 0; n < mutations; ++n) {
            const auto offset = random() % (source.size() + 1);
            const auto operation = random() % 4;
            if (operation == 0 && offset < source.size()) source.erase(offset, 1 + random() % 8);
            else if (operation == 1 && offset < source.size()) source[offset] = static_cast<char>(random() % 256);
            else if (operation == 2) source.insert(offset, 1, static_cast<char>(random() % 128));
            else if (offset < source.size()) source.insert(offset, source.substr(offset, 1 + random() % 16));
        }
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(source.data()), source.size());
    }
    std::cout << "OK: 20000 deterministic source mutations\n";
}
#endif
