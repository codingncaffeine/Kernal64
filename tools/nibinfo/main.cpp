// nibinfo: the report the core logs when it loads a NIB disk image, for a file on its own.
// For each track: how its revolution was found in the raw read, its length, its SYNCs and the
// sector headers and data blocks it holds, with good or bad checksums; then a summary.
//
//   nibinfo FILE.nib...
#include "disk.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: nibinfo FILE.nib...\n"); return 2; }
    int rc = 0;
    for (int a = 1; a < argc; ++a) {
        std::ifstream f(argv[a], std::ios::binary);
        if (!f) { std::fprintf(stderr, "%s: cannot read\n", argv[a]); rc = 1; continue; }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::printf("== %s\n", argv[a]);
        for (const auto& line : k64::describeNib(bytes.data(), bytes.size())) std::printf("%s\n", line.c_str());
    }
    return rc;
}
