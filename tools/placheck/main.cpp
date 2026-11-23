// placheck: compares the PLA logic (src/core/pla.h) with a 64 KB read-out of a real
// PLA, indexed by the 16 inputs in the same bit order, for every combination.
//
//   placheck PLA.BIN [--max-show N]
#include "pla.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: placheck PLA.BIN [--max-show N]\n"); return 2; }
    int maxShow = 10;
    if (argc >= 4 && !std::strcmp(argv[2], "--max-show")) maxShow = std::atoi(argv[3]);
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<unsigned char> dump((std::istreambuf_iterator<char>(f)), {});
    if (dump.size() != 65536) { std::fprintf(stderr, "expected 65536 bytes, got %zu\n", dump.size()); return 2; }
    static const char* names[8] = {"#ROML", "#IO", "#GRW", "#CHARROM", "#KERNAL", "#BASIC", "#CASRAM", "#ROMH"};
    long differ = 0;
    long perOutput[8] = {};
    for (unsigned i = 0; i < 65536; ++i) {
        const unsigned char ours = k64::pla::evaluate(static_cast<uint16_t>(i));
        const unsigned char real = dump[i];
        if (ours == real) continue;
        ++differ;
        for (int b = 0; b < 8; ++b) if ((ours ^ real) & (1 << b)) ++perOutput[b];
        if (differ <= maxShow) std::printf("inputs %04x: ours %02x, chip %02x\n", i, ours, real);
    }
    for (int b = 0; b < 8; ++b) if (perOutput[b]) std::printf("  %s differs in %ld combinations\n", names[b], perOutput[b]);
    std::printf("placheck: %ld of 65536 input combinations differ\n", differ);
    return differ ? 1 : 0;
}
