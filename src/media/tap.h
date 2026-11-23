// A tape as the datasette's head reads it: the lengths, in machine cycles, of the pulses one
// after another (from one falling edge of the read signal to the next). TAP versions 0 and 1
// in and version 1 out. Rules and sources: local-notes/hw/tape.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k64 {

struct Tape {
    std::vector<uint32_t> pulses;   // cycles per pulse
    bool changed = false;           // the datasette recorded onto it since it was made or saved
};

// "C64-TAPE-RAW", version 0 or 1 (T1-T3). False, with the reason in `error`, for anything else.
bool parseTap(const uint8_t* data, size_t size, Tape& out, std::string& error);
// Version 1, for a C64 of the given video standard (0 PAL, 1 NTSC, 2 old NTSC) (T4).
std::vector<uint8_t> writeTap(const Tape& tape, int videoStandard);

} // namespace k64
