// The C64's keys: matrix position (Programmer's Reference Guide: column = CIA 1 port A bit,
// row = port B bit), whether SHIFT goes with it (keys the C64 reaches only shifted), the
// option value that names it and its label. RESTORE is not in the matrix (row -1).
#pragma once

#include <cstdint>

namespace k64lr {

struct C64Key {
    const char* id;      // option value, "key_" + id
    const char* label;   // option label
    int8_t col, row;
    bool shift;
};

inline constexpr C64Key kC64Keys[] = {
    {"return", "RETURN", 0, 1, false}, {"space", "SPACE", 7, 4, false}, {"runstop", "RUN/STOP", 7, 7, false},
    {"f1", "F1", 0, 4, false}, {"f2", "F2", 0, 4, true}, {"f3", "F3", 0, 5, false}, {"f4", "F4", 0, 5, true},
    {"f5", "F5", 0, 6, false}, {"f6", "F6", 0, 6, true}, {"f7", "F7", 0, 3, false}, {"f8", "F8", 0, 3, true},
    {"crsr_down", "CRSR down", 0, 7, false}, {"crsr_up", "CRSR up", 0, 7, true},
    {"crsr_right", "CRSR right", 0, 2, false}, {"crsr_left", "CRSR left", 0, 2, true},
    {"lshift", "Left SHIFT", 1, 7, false}, {"rshift", "Right SHIFT", 6, 4, false}, {"cbm", "Commodore key", 7, 5, false},
    {"ctrl", "CTRL", 7, 2, false}, {"restore", "RESTORE", -1, -1, false}, {"insdel", "INST/DEL", 0, 0, false},
    {"clrhome", "CLR/HOME", 6, 3, false},
    {"1", "1", 7, 0, false}, {"2", "2", 7, 3, false}, {"3", "3", 1, 0, false}, {"4", "4", 1, 3, false},
    {"5", "5", 2, 0, false}, {"6", "6", 2, 3, false}, {"7", "7", 3, 0, false}, {"8", "8", 3, 3, false},
    {"9", "9", 4, 0, false}, {"0", "0", 4, 3, false},
    {"a", "A", 1, 2, false}, {"b", "B", 3, 4, false}, {"c", "C", 2, 4, false}, {"d", "D", 2, 2, false},
    {"e", "E", 1, 6, false}, {"f", "F", 2, 5, false}, {"g", "G", 3, 2, false}, {"h", "H", 3, 5, false},
    {"i", "I", 4, 1, false}, {"j", "J", 4, 2, false}, {"k", "K", 4, 5, false}, {"l", "L", 5, 2, false},
    {"m", "M", 4, 4, false}, {"n", "N", 4, 7, false}, {"o", "O", 4, 6, false}, {"p", "P", 5, 1, false},
    {"q", "Q", 7, 6, false}, {"r", "R", 2, 1, false}, {"s", "S", 1, 5, false}, {"t", "T", 2, 6, false},
    {"u", "U", 3, 6, false}, {"v", "V", 3, 7, false}, {"w", "W", 1, 1, false}, {"x", "X", 2, 7, false},
    {"y", "Y", 3, 1, false}, {"z", "Z", 1, 4, false},
    {"plus", "+", 5, 0, false}, {"minus", "-", 5, 3, false}, {"pound", "Pound", 6, 0, false},
    {"at", "@", 5, 6, false}, {"star", "*", 6, 1, false}, {"uparrow", "Up arrow", 6, 6, false},
    {"leftarrow", "Left arrow", 7, 1, false}, {"colon", ":", 5, 5, false}, {"semicolon", ";", 6, 2, false},
    {"equals", "=", 6, 5, false}, {"comma", ",", 5, 7, false}, {"period", ".", 5, 4, false}, {"slash", "/", 6, 7, false},
};
inline constexpr int kC64KeyCount = static_cast<int>(sizeof kC64Keys / sizeof kC64Keys[0]);

// Index of the key with option id `id` (without "key_"), or -1.
int keyIndex(const char* id);

} // namespace k64lr
