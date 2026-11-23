// Single-file programs: a plain program file (two bytes of load address, then the bytes),
// the same inside a P00 wrapper, or the first program of a T64 container.
// Formats: local-notes/hw/cart.md M1-M3.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k64 {

struct ProgramFile {
    uint16_t load = 0;            // where the bytes go
    std::vector<uint8_t> data;    // without the load address
    std::string name;             // as stored in the container (PETSCII), if it has one
};

// Recognizes the three forms by their content. False, with the reason in `error`, when the
// bytes are none of them or hold no program.
bool parseProgram(const uint8_t* data, size_t size, ProgramFile& out, std::string& error);

} // namespace k64
