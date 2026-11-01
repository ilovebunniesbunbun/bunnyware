#include <iostream>
#include <string>
#include "src/vpk/vpk.hpp"

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    vpk::VPKDir vpk;
    if (!vpk.open(argv[1])) {
        std::cerr << "Failed to open vpk" << std::endl;
        return 1;
    }
    for (const auto& [path, entry] : vpk.entries_) {
        std::cout << path << std::endl;
    }
    return 0;
}
