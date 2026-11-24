// The core options: one table, handed to the frontend as options v2 (with categories), v1 or
// plain variables, whichever it takes.
#pragma once

#include "libretro.h"

namespace k64lr {

// The RetroPad buttons whose job is an option ("kernal64_pad_" + name).
struct PadButton { const char* name; unsigned id; const char* label; const char* defaultValue; };
inline constexpr PadButton kPadButtons[] = {
    {"b", RETRO_DEVICE_ID_JOYPAD_B, "RetroPad B", "fire"},
    {"a", RETRO_DEVICE_ID_JOYPAD_A, "RetroPad A", "none"},
    {"y", RETRO_DEVICE_ID_JOYPAD_Y, "RetroPad Y", "none"},
    {"x", RETRO_DEVICE_ID_JOYPAD_X, "RetroPad X", "key_space"},
    {"l", RETRO_DEVICE_ID_JOYPAD_L, "RetroPad L", "none"},
    {"r", RETRO_DEVICE_ID_JOYPAD_R, "RetroPad R", "none"},
    {"l2", RETRO_DEVICE_ID_JOYPAD_L2, "RetroPad L2", "key_runstop"},
    {"r2", RETRO_DEVICE_ID_JOYPAD_R2, "RetroPad R2", "key_return"},
    {"l3", RETRO_DEVICE_ID_JOYPAD_L3, "RetroPad L3", "none"},
    {"r3", RETRO_DEVICE_ID_JOYPAD_R3, "RetroPad R3", "freeze"},
    {"select", RETRO_DEVICE_ID_JOYPAD_SELECT, "RetroPad Select", "keyboard"},
    {"start", RETRO_DEVICE_ID_JOYPAD_START, "RetroPad Start", "none"},
};
inline constexpr int kPadButtonCount = static_cast<int>(sizeof kPadButtons / sizeof kPadButtons[0]);

// What a mapped button does; a key is kKey + its index in kC64Keys.
enum PadAction : int {
    kNone, kFire, kTurbo, kUp, kKeyboard, kTapePlay, kTapeRecord, kTapeRewind, kFreeze, kSwapPorts, kKey = 100,
};
PadAction padActionFromValue(const char* value);

// Registers the options with the frontend (call from retro_set_environment).
void registerOptions(retro_environment_t env);

} // namespace k64lr
