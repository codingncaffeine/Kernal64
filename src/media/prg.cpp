#include "prg.h"

#include <cstring>

namespace k64 {

namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24); }

bool plain(const uint8_t* data, size_t size, ProgramFile& out, std::string& error) {
    if (size < 3) { error = "program file without a program"; return false; }
    out.load = le16(data);
    // What does not fit below the top of memory is not loaded.
    const size_t room = 0x10000 - out.load;
    out.data.assign(data + 2, data + 2 + (size - 2 < room ? size - 2 : room));
    return true;
}

// M3: a 64-byte tape record, then 32-byte file records, then the files' bytes.
bool container(const uint8_t* data, size_t size, ProgramFile& out, std::string& error) {
    if (size < 64 + 32) { error = "tape container without a file record"; return false; }
    const unsigned entries = le16(data + 34) ? le16(data + 34) : 1;   // some files say 0 and hold one
    for (unsigned i = 0; i < entries && 64 + 32 * (size_t{i} + 1) <= size; ++i) {
        const uint8_t* e = data + 64 + 32 * size_t{i};
        if (e[0] != 1 || e[1] == 0) continue;    // only "normal tape file" records hold a program
        const uint16_t start = le16(e + 2), end = le16(e + 4);
        const size_t offset = le32(e + 8);
        if (offset >= size) continue;
        // The bytes the container really holds for this record: up to the next record's
        // bytes or the end of the file. Files made by one old tool give $C3C6 as the end
        // address of everything: a length that does not fit is taken from the container.
        size_t available = size - offset;
        for (unsigned j = 0; j < entries && 64 + 32 * (size_t{j} + 1) <= size; ++j) {
            const uint8_t* o = data + 64 + 32 * size_t{j};
            const size_t other = le32(o + 8);
            if (o[0] == 1 && other > offset && other - offset < available) available = other - offset;
        }
        size_t length = end > start ? size_t{end} - start : 0;
        if (length == 0 || length > available) length = available;
        const size_t room = 0x10000 - start;
        if (length > room) length = room;
        if (length == 0) continue;
        out.load = start;
        out.data.assign(data + offset, data + offset + length);
        out.name.assign(reinterpret_cast<const char*>(e + 16), 16);
        while (!out.name.empty() && out.name.back() == ' ') out.name.pop_back();
        return true;
    }
    error = "tape container without a program";
    return false;
}

} // namespace

bool parseProgram(const uint8_t* data, size_t size, ProgramFile& out, std::string& error) {
    out = ProgramFile{};
    // M2: "C64File", a zero, 16 bytes of name, a zero, a record size; then the program file.
    if (size >= 26 && std::memcmp(data, "C64File", 8) == 0) {
        if (!plain(data + 26, size - 26, out, error)) return false;
        for (size_t i = 8; i < 24 && data[i]; ++i) out.name += static_cast<char>(data[i]);
        return true;
    }
    // M3: the tape record starts with a text that begins "C64" ("C64S tape image file",
    // "C64 tape image file" and others). A plain program loading at $3643 ("C6") whose
    // first byte is '4' would look the same: the record's version field tells them apart.
    if (size >= 64 && std::memcmp(data, "C64", 3) == 0 && data[33] <= 2 && (le16(data + 34) <= 4096))
        return container(data, size, out, error);
    return plain(data, size, out, error);
}

} // namespace k64
