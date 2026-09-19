#include "gcam_rawpack.h"

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: rawpack_dump <frame.rawpack>\n";
        return 2;
    }
    try {
        std::cout << gcam::rawpack_metadata_text(gcam::read_rawpack(argv[1]));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rawpack_dump error: " << error.what() << '\n';
        return 1;
    }
}
