// libretro entry points.
#include "../core/c64.h"
#include "../core/roms.h"
#include "libretro.h"
#include "keys.h"
#include "options.h"
#include "osk.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef KERNAL64_VERSION
#define KERNAL64_VERSION "0.0.0"
#endif

namespace {

retro_environment_t envCb;
retro_video_refresh_t videoCb;
retro_audio_sample_t audioSampleCb;
retro_audio_sample_batch_t audioBatchCb;
retro_input_poll_t inputPollCb;
retro_input_state_t inputStateCb;
retro_log_printf_t logCb;
retro_led_interface ledCb{};
bool ledWas = false;

std::unique_ptr<k64::C64> machine;
// The disks: one image, or the list an .m3u names (swapped through the frontend's disk
// control). What the drive writes on one is kept (PLAN D10) in a G64 copy in the frontend's
// save folder, named after the image; the user's files are never written.
struct DiskSlot {
    std::string path;                        // the image file
    std::string savePath;                    // its kept copy
    std::shared_ptr<k64::GcrDisk> disk;      // read when first inserted
};
std::vector<DiskSlot> slots;
size_t slotIndex = 0;
bool diskEjected = false;
std::shared_ptr<k64::GcrDisk> disk;          // the one in the drive (none while ejected)
// The tape in the datasette and its kept copy (what the datasette recorded; PLAN D10).
std::shared_ptr<k64::Tape> tape;
std::string tapeSavePath;

std::vector<uint32_t> frameOut;
std::vector<int16_t> audioOut;
std::vector<float> audioMono;

constexpr unsigned kSampleRate = 48000;


void log(retro_log_level level, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (logCb) logCb(level, "[Kernal64] %s\n", buf);
    else std::fprintf(stderr, "[Kernal64] %s\n", buf);
}

// ---- input -------------------------------------------------------------------------
//
// The host keyboard maps onto the C64 matrix by position (Programmer's Reference Guide
// matrix: column = CIA 1 port A bit, row = port B bit). Keys the C64 reaches only with
// SHIFT (cursor left/up, F2/F4/F6/F8) press SHIFT with them. RESTORE is not in the matrix.
struct KeyMap { unsigned key; int8_t col, row; bool shift; };
constexpr KeyMap kKeys[] = {
    {RETROK_BACKSPACE, 0, 0, false}, {RETROK_RETURN, 0, 1, false}, {RETROK_KP_ENTER, 0, 1, false},
    {RETROK_RIGHT, 0, 2, false}, {RETROK_LEFT, 0, 2, true}, {RETROK_DOWN, 0, 7, false}, {RETROK_UP, 0, 7, true},
    {RETROK_F7, 0, 3, false}, {RETROK_F8, 0, 3, true}, {RETROK_F1, 0, 4, false}, {RETROK_F2, 0, 4, true},
    {RETROK_F3, 0, 5, false}, {RETROK_F4, 0, 5, true}, {RETROK_F5, 0, 6, false}, {RETROK_F6, 0, 6, true},
    {RETROK_3, 1, 0, false}, {RETROK_w, 1, 1, false}, {RETROK_a, 1, 2, false}, {RETROK_4, 1, 3, false},
    {RETROK_z, 1, 4, false}, {RETROK_s, 1, 5, false}, {RETROK_e, 1, 6, false}, {RETROK_LSHIFT, 1, 7, false},
    {RETROK_5, 2, 0, false}, {RETROK_r, 2, 1, false}, {RETROK_d, 2, 2, false}, {RETROK_6, 2, 3, false},
    {RETROK_c, 2, 4, false}, {RETROK_f, 2, 5, false}, {RETROK_t, 2, 6, false}, {RETROK_x, 2, 7, false},
    {RETROK_7, 3, 0, false}, {RETROK_y, 3, 1, false}, {RETROK_g, 3, 2, false}, {RETROK_8, 3, 3, false},
    {RETROK_b, 3, 4, false}, {RETROK_h, 3, 5, false}, {RETROK_u, 3, 6, false}, {RETROK_v, 3, 7, false},
    {RETROK_9, 4, 0, false}, {RETROK_i, 4, 1, false}, {RETROK_j, 4, 2, false}, {RETROK_0, 4, 3, false},
    {RETROK_m, 4, 4, false}, {RETROK_k, 4, 5, false}, {RETROK_o, 4, 6, false}, {RETROK_n, 4, 7, false},
    {RETROK_MINUS, 5, 0, false},        // +
    {RETROK_p, 5, 1, false}, {RETROK_l, 5, 2, false},
    {RETROK_EQUALS, 5, 3, false},       // -
    {RETROK_PERIOD, 5, 4, false},
    {RETROK_SEMICOLON, 5, 5, false},    // :
    {RETROK_LEFTBRACKET, 5, 6, false},  // @
    {RETROK_COMMA, 5, 7, false},
    {RETROK_INSERT, 6, 0, false},       // pound
    {RETROK_RIGHTBRACKET, 6, 1, false}, // *
    {RETROK_QUOTE, 6, 2, false},        // ;
    {RETROK_HOME, 6, 3, false},         // CLR/HOME
    {RETROK_RSHIFT, 6, 4, false},
    {RETROK_BACKSLASH, 6, 5, false},    // =
    {RETROK_DELETE, 6, 6, false},       // up arrow
    {RETROK_SLASH, 6, 7, false},
    {RETROK_1, 7, 0, false}, {RETROK_BACKQUOTE, 7, 1, false},  // left arrow
    {RETROK_TAB, 7, 2, false},          // CTRL
    {RETROK_2, 7, 3, false}, {RETROK_SPACE, 7, 4, false},
    {RETROK_LCTRL, 7, 5, false},        // Commodore key
    {RETROK_q, 7, 6, false},
    {RETROK_ESCAPE, 7, 7, false},       // RUN/STOP
};

// Symbolic keyboard (option kernal64_keyboard, the default): a key types the symbol printed on it on
// a US keyboard. Digits and punctuation go to the C64 key that makes that symbol, with or without the
// C64's SHIFT as that symbol needs (the C64 has " on SHIFT+2, * and + on keys of their own, : and ;
// unshifted ...). The host's SHIFT then only says which of the key's two symbols is meant. Letters and
// the other keys keep their place (SHIFT+letter is the C64's own graphics character).
struct SymbolKey { unsigned key; int8_t col, row; bool shift; int8_t scol, srow; bool sshift; };
constexpr SymbolKey kSymbols[] = {
    // host key       unshifted: C64 key, SHIFT     shifted: C64 key, SHIFT
    {RETROK_1,         7, 0, false,                   7, 0, true},    // 1  !
    {RETROK_2,         7, 3, false,                   5, 6, false},   // 2  @
    {RETROK_3,         1, 0, false,                   1, 0, true},    // 3  #
    {RETROK_4,         1, 3, false,                   1, 3, true},    // 4  $
    {RETROK_5,         2, 0, false,                   2, 0, true},    // 5  %
    {RETROK_6,         2, 3, false,                   6, 6, false},   // 6  ^ = the up arrow
    {RETROK_7,         3, 0, false,                   2, 3, true},    // 7  & = SHIFT+6
    {RETROK_8,         3, 3, false,                   6, 1, false},   // 8  *
    {RETROK_9,         4, 0, false,                   3, 3, true},    // 9  ( = SHIFT+8
    {RETROK_0,         4, 3, false,                   4, 0, true},    // 0  ) = SHIFT+9
    {RETROK_MINUS,     5, 3, false,                   7, 1, false},   // -  _ = the left arrow
    {RETROK_EQUALS,    6, 5, false,                   5, 0, false},   // =  +
    {RETROK_LEFTBRACKET, 5, 5, true,                  5, 5, true},    // [ = SHIFT+:
    {RETROK_RIGHTBRACKET, 6, 2, true,                 6, 2, true},    // ] = SHIFT+;
    {RETROK_BACKSLASH, 6, 0, false,                   6, 0, false},   // \ = the pound key
    {RETROK_SEMICOLON, 6, 2, false,                   5, 5, false},   // ;  :
    {RETROK_QUOTE,     3, 0, true,                    7, 3, true},    // ' = SHIFT+7, " = SHIFT+2
    {RETROK_BACKQUOTE, 7, 1, false,                   7, 1, false},   // ` = the left arrow
    {RETROK_COMMA,     5, 7, false,                   5, 7, true},    // ,  <
    {RETROK_PERIOD,    5, 4, false,                   5, 4, true},    // .  >
    {RETROK_SLASH,     6, 7, false,                   6, 7, true},    // /  ?
};

bool symbolicKeyboard() {
    retro_variable var{"kernal64_keyboard", nullptr};
    return !(envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !std::strcmp(var.value, "positional"));
}
bool symbolicKeys = true;   // read with the options

int joystickPort() {
    retro_variable var{"kernal64_joyport", nullptr};
    if (envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !std::strcmp(var.value, "1")) return 1;
    return 2;
}

void notify(const std::string& line);

// ---- the players' controllers ----------------------------------------------------------
//
// Player 1 and 2 (libretro ports 0 and 1) each have a device in a C64 control port: player
// 1's in the port the kernal64_joyport option names, player 2's in the other (a "swap" button
// exchanges them while running). Devices and their wiring: local-notes/hw/input.md.
constexpr unsigned kDevicePaddles = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0);
unsigned device[2] = {RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};
bool portsSwapped = false;
int portOf(int player) {
    int p = joystickPort();
    if (player == 1) p = 3 - p;
    return portsSwapped ? 3 - p : p;
}

k64lr::PadAction padMap[k64lr::kPadButtonCount];
// Fast loading: what counts as loading (off, the tape, the tape and the 1541).
enum class FastLoading { Off, Tape, TapeAndDisk };
FastLoading fastLoading = FastLoading::TapeAndDisk;
constexpr int kFastFactor = 4;
double audioDue = 0.0;     // samples due since the content was loaded (frames shown x rate / fps)
uint64_t audioSent = 0;    // samples handed over
bool loadingNow() {
    if (fastLoading == FastLoading::Off) return false;
    if (machine->tapeMoving() || machine->kernalTapePause()) return true;
    return fastLoading == FastLoading::TapeAndDisk && machine->driveAttached() && machine->drive().motor();
}

void readPadOptions() {
    symbolicKeys = symbolicKeyboard();
    {
        retro_variable var{"kernal64_fast_loading", nullptr};
        const bool got = envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value;
        fastLoading = !got || !std::strcmp(var.value, "tape_disk") ? FastLoading::TapeAndDisk
                    : !std::strcmp(var.value, "tape") ? FastLoading::Tape : FastLoading::Off;
    }
    for (int b = 0; b < k64lr::kPadButtonCount; ++b) {
        const std::string key = std::string("kernal64_pad_") + k64lr::kPadButtons[b].name;
        retro_variable var{key.c_str(), nullptr};
        const bool got = envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value;
        padMap[b] = k64lr::padActionFromValue(got ? var.value : k64lr::kPadButtons[b].defaultValue);
    }
}

k64lr::Osk osk;
int oskOwner = 0;
bool padWas[2][16] = {};
int mouseAcc[2][2] = {}, mousePos[2][2] = {};   // per player: movement not yet passed on, 1351 position
struct Crop { int x = 0, y = 0, w = 0, h = 0; };
Crop crop;   // the part of the visible picture that is shown (kernal64_border)

void tapeButton(k64lr::PadAction a) {
    auto& d = machine->datasette();
    if (!d.hasTape()) { notify("Datasette: no tape"); return; }
    if (a == k64lr::kTapePlay) {
        const bool moving = d.button() != k64::Datasette::Button::None;
        if (moving) d.stop(); else d.press(k64::Datasette::Button::Play);
        notify(moving ? "Datasette: STOP" : "Datasette: PLAY");
    } else if (a == k64lr::kTapeRecord) {
        d.press(k64::Datasette::Button::Record);
        notify("Datasette: RECORD");
    } else {
        d.rewind();
        notify("Datasette: rewound");
    }
}

void applyInput() {
    if (!inputStateCb) return;
    machine->releaseAllKeys();
    std::array<bool, k64lr::kC64KeyCount> keys{};
    auto hostKey = [](unsigned key) { return inputStateCb(0, RETRO_DEVICE_KEYBOARD, 0, key) != 0; };
    const bool hostShift = hostKey(RETROK_LSHIFT) || hostKey(RETROK_RSHIFT);
    int symbolShift = -1;   // a symbol key is down: the C64's SHIFT as its symbol needs (0 / 1)
    if (symbolicKeys) {
        for (const SymbolKey& k : kSymbols) {
            if (!hostKey(k.key)) continue;
            machine->setKey(hostShift ? k.scol : k.col, hostShift ? k.srow : k.row, true);
            symbolShift = (hostShift ? k.sshift : k.shift) ? 1 : 0;
        }
    }
    for (const auto& k : kKeys) {
        if (!hostKey(k.key)) continue;
        const bool isShift = k.key == RETROK_LSHIFT || k.key == RETROK_RSHIFT;
        if (symbolicKeys && (isShift || std::any_of(std::begin(kSymbols), std::end(kSymbols), [&](const SymbolKey& s) { return s.key == k.key; })))
            continue;   // the symbol table and the SHIFT decision below handle these
        machine->setKey(k.col, k.row, true);
        if (k.shift) machine->setKey(1, 7, true);
    }
    if (symbolicKeys && (symbolShift == 1 || (symbolShift < 0 && hostShift))) machine->setKey(1, 7, true);
    bool restore = inputStateCb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_PAGEUP) != 0;
    // The button on a freezer cartridge: Page Down, or a RetroPad button mapped to it.
    bool freeze = inputStateCb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_PAGEDOWN) != 0;
    uint8_t lines[2] = {0, 0};
    int pots[2][2] = {{-1, -1}, {-1, -1}};
    int penLine = -1, penCycle = 0;
    for (int player = 0; player < 2; ++player) {
        const unsigned dev = device[player];
        if (dev == RETRO_DEVICE_NONE) continue;
        const int port = portOf(player);
        bool now[16];
        for (unsigned id = 0; id < 16; ++id) now[id] = inputStateCb(static_cast<unsigned>(player), RETRO_DEVICE_JOYPAD, 0, id) != 0;
        auto pressed = [&](unsigned id) { return now[id] && !padWas[player][id]; };
        const bool oskHere = osk.isOpen() && oskOwner == player;
        bool fire = false, up = false;
        for (int b = 0; b < k64lr::kPadButtonCount; ++b) {
            const unsigned id = k64lr::kPadButtons[b].id;
            const k64lr::PadAction a = padMap[b];
            if (oskHere && a != k64lr::kKeyboard) continue;   // the keyboard has this pad
            switch (a) {
            case k64lr::kNone: break;
            case k64lr::kFire: fire |= now[id]; break;
            case k64lr::kTurbo: fire |= now[id] && (machine->frame() / 2) % 2 == 0; break;
            case k64lr::kUp: up |= now[id]; break;
            case k64lr::kKeyboard:
                if (pressed(id)) {
                    osk.setOpen(!osk.isOpen());
                    oskOwner = player;
                }
                break;
            case k64lr::kTapePlay: case k64lr::kTapeRecord: case k64lr::kTapeRewind:
                if (pressed(id)) tapeButton(a);
                break;
            case k64lr::kFreeze: freeze |= now[id]; break;
            case k64lr::kSwapPorts:
                if (pressed(id)) {
                    portsSwapped = !portsSwapped;
                    notify("Player 1 in control port " + std::to_string(portOf(0)));
                }
                break;
            default:
                if (a >= k64lr::kKey) keys[static_cast<size_t>(a - k64lr::kKey)] |= now[id];
                break;
            }
        }
        if (oskHere) {
            using J = unsigned;
            osk.update({now[J{RETRO_DEVICE_ID_JOYPAD_UP}], now[J{RETRO_DEVICE_ID_JOYPAD_DOWN}], now[J{RETRO_DEVICE_ID_JOYPAD_LEFT}],
                        now[J{RETRO_DEVICE_ID_JOYPAD_RIGHT}], now[J{RETRO_DEVICE_ID_JOYPAD_B}], now[J{RETRO_DEVICE_ID_JOYPAD_X}],
                        now[J{RETRO_DEVICE_ID_JOYPAD_START}], now[J{RETRO_DEVICE_ID_JOYPAD_Y}], now[J{RETRO_DEVICE_ID_JOYPAD_A}]});
        }
        std::copy(now, now + 16, padWas[player]);
        auto analog = [&](unsigned index, unsigned id) {
            return inputStateCb(static_cast<unsigned>(player), RETRO_DEVICE_ANALOG, index, id);
        };
        uint8_t l = static_cast<uint8_t>((up ? 0x01 : 0) | (fire ? 0x10 : 0));
        if (dev == RETRO_DEVICE_JOYPAD) {
            if (!oskHere) {
                // The D-pad, or the left stick pushed past half way.
                const int ax = analog(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
                const int ay = analog(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
                l |= static_cast<uint8_t>((now[RETRO_DEVICE_ID_JOYPAD_UP] || ay < -0x4000 ? 0x01 : 0) |
                                          (now[RETRO_DEVICE_ID_JOYPAD_DOWN] || ay > 0x4000 ? 0x02 : 0) |
                                          (now[RETRO_DEVICE_ID_JOYPAD_LEFT] || ax < -0x4000 ? 0x04 : 0) |
                                          (now[RETRO_DEVICE_ID_JOYPAD_RIGHT] || ax > 0x4000 ? 0x08 : 0));
            } else {
                l = 0;
            }
        } else if (dev == kDevicePaddles) {
            // P1-P3: a stick's X is a knob (right = clockwise = lower); the button mapped to fire
            // is paddle X's (LEFT line), A paddle Y's (RIGHT line). With paddles for both
            // players, player 2 turns paddle Y of player 1's port.
            auto knob = [](int x) { return 255 - (x + 32768) * 255 / 65535; };
            const int lx = analog(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
            const int rx = analog(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
            const bool second = player == 1 && device[0] == kDevicePaddles;
            const int p = (second ? portOf(0) : port) - 1;
            if (second) {
                pots[p][1] = knob(lx);
                if (fire) lines[p] |= 0x08;
                l = 0;
            } else {
                pots[p][0] = knob(lx);
                if (pots[p][1] < 0) pots[p][1] = knob(rx);
                l = static_cast<uint8_t>((fire ? 0x04 : 0) | (now[RETRO_DEVICE_ID_JOYPAD_A] ? 0x08 : 0));
            }
        } else if (dev == RETRO_DEVICE_MOUSE) {
            // M1-M4: the position modulo 64 in pot bits 6..1, at most 31 steps a frame; Y counts
            // up as the mouse moves away from the user. Left button = FIRE, right = UP.
            auto m = [&](unsigned id) { return inputStateCb(static_cast<unsigned>(player), RETRO_DEVICE_MOUSE, 0, id); };
            mouseAcc[player][0] += m(RETRO_DEVICE_ID_MOUSE_X);
            mouseAcc[player][1] -= m(RETRO_DEVICE_ID_MOUSE_Y);
            for (int axis = 0; axis < 2; ++axis) {
                const int step = std::clamp(mouseAcc[player][axis], -31, 31);
                mouseAcc[player][axis] -= step;
                mousePos[player][axis] += step;
                pots[port - 1][axis] = (mousePos[player][axis] & 63) << 1;
            }
            l |= static_cast<uint8_t>((m(RETRO_DEVICE_ID_MOUSE_LEFT) ? 0x10 : 0) | (m(RETRO_DEVICE_ID_MOUSE_RIGHT) ? 0x01 : 0));
        } else if (dev == RETRO_DEVICE_LIGHTGUN && port == 1) {
            // L1-L3: the pen looks at a point of the shown picture; its button is on UP.
            auto g = [&](unsigned id) { return inputStateCb(static_cast<unsigned>(player), RETRO_DEVICE_LIGHTGUN, 0, id); };
            const auto& t = machine->timing();
            if (!g(RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) && crop.w > 0) {
                const int x = (g(RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X) + 0x7FFF) * crop.w / 0xFFFF;
                const int y = (g(RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y) + 0x7FFF) * crop.h / 0xFFFF;
                if (x >= 0 && x < crop.w && y >= 0 && y < crop.h) {
                    penLine = (t.firstVisibleLine + crop.y + y) % t.linesPerFrame;
                    penCycle = (t.firstVisiblePixel + crop.x + x) / 8 + 1;
                }
            }
            if (g(RETRO_DEVICE_ID_LIGHTGUN_TRIGGER)) l |= 0x01;
        }
        lines[port - 1] |= l;
    }
    osk.keysDown(keys);
    for (int k = 0; k < k64lr::kC64KeyCount; ++k) {
        if (!keys[static_cast<size_t>(k)]) continue;
        const k64lr::C64Key& key = k64lr::kC64Keys[k];
        if (key.row < 0) { restore = true; continue; }
        machine->setKey(key.col, key.row, true);
        if (key.shift) machine->setKey(1, 7, true);
    }
    machine->setRestore(restore);
    machine->setFreezeButton(freeze);
    machine->setJoystick(1, lines[0]);
    machine->setJoystick(2, lines[1]);
    machine->setPaddles(1, pots[0][0], pots[0][1]);
    machine->setPaddles(2, pots[1][0], pots[1][1]);
    machine->setLightPen(penLine, penCycle);
}

// The content's file name, for the automatic model.
std::string contentName;

// "Automatic": NTSC when the file name says so ("NTSC", or a region tag of an NTSC country
// alone: "(USA)", "(Japan)"), else PAL, the machine most C64 software was made on.
k64::VicModel modelForName(const std::string& path) {
    std::string n = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool ntsc = n.find("ntsc") != std::string::npos || n.find("(usa)") != std::string::npos || n.find("(japan)") != std::string::npos;
    return ntsc ? k64::VicModel::Mos6567R8 : k64::VicModel::Mos6569;
}

k64::VicModel modelFromOptions() {
    retro_variable var{"kernal64_model", nullptr};
    k64::VicModel m = modelForName(contentName);
    if (envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) k64::vicModelFromName(var.value, m);
    return m;
}

// What each RetroPad button does, for the frontend's remapping screen.
void setDescriptors() {
    static std::vector<retro_input_descriptor> d;
    static std::vector<std::string> labels;
    d.clear();
    labels.clear();
    labels.reserve(2 * k64lr::kPadButtonCount);
    for (unsigned p = 0; p < 2; ++p) {
        d.push_back({p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick up"});
        d.push_back({p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick down"});
        d.push_back({p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick left"});
        d.push_back({p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick right"});
        d.push_back({p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Joystick / paddle X"});
        d.push_back({p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Joystick"});
        d.push_back({p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Paddle Y"});
        for (int b = 0; b < k64lr::kPadButtonCount; ++b) {
            const k64lr::PadAction a = padMap[b];
            if (a == k64lr::kNone) continue;
            static const char* kNames[] = {"", "Joystick fire", "Joystick fire, repeating", "Joystick up", "On-screen keyboard",
                                           "Datasette PLAY / STOP", "Datasette RECORD", "Datasette rewind",
                                           "Freezer cartridge button", "Swap the joystick ports"};
            labels.push_back(a >= k64lr::kKey ? std::string("Key ") + k64lr::kC64Keys[a - k64lr::kKey].label
                                              : std::string(kNames[static_cast<int>(a)]));
            d.push_back({p, RETRO_DEVICE_JOYPAD, 0, k64lr::kPadButtons[b].id, labels.back().c_str()});
        }
    }
    d.push_back({0, 0, 0, 0, nullptr});
    envCb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, d.data());
}

k64::CiaModel ciaFromOptions() {
    retro_variable var{"kernal64_cia", nullptr};
    if (envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !std::strcmp(var.value, "8521"))
        return k64::CiaModel::Mos8521;
    return k64::CiaModel::Mos6526;
}

// A way in which 6581s differ; the default is what most do (hw/sid.md W10a).
bool sidNoisePulseDrainsFromOptions() {
    retro_variable var{"kernal64_sid_noise_pulse", nullptr};
    return !(envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !std::strcmp(var.value, "leaves it alone"));
}

bool driveFromOptions() {
    retro_variable var{"kernal64_drive8", nullptr};
    return !(envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !std::strcmp(var.value, "off"));
}

k64::SidModel sidFromOptions() {
    retro_variable var{"kernal64_sid", nullptr};
    if (envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !std::strcmp(var.value, "8580"))
        return k64::SidModel::Mos8580;
    return k64::SidModel::Mos6581;
}

// Border option: "full" shows everything the chip sends between the blanking intervals,
// "small" the 320x200 window with up to 32 pixels of border left and right and 36 lines
// above and below (as much of that as the model's picture has, the same on both sides),
// "none" the window alone. The window starts at X coordinate $18, raster line 51 (40
// columns, 25 lines).
Crop cropFromOptions() {
    const auto& t = machine->timing();
    Crop full{0, 0, t.visibleWidth, t.visibleLines};
    retro_variable var{"kernal64_border", nullptr};
    if (!envCb || !envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value || !std::strcmp(var.value, "full")) return full;
    int windowPixel = 0;
    while (windowPixel < 8 * t.cyclesPerLine && machine->vic().xAt(windowPixel) != 0x18) ++windowPixel;
    const int x0 = windowPixel - t.firstVisiblePixel, y0 = 51 - t.firstVisibleLine;
    const int right = t.visibleWidth - x0 - 320, below = t.visibleLines - y0 - 200;
    if (x0 < 0 || y0 < 0 || right < 0 || below < 0) return full;
    const bool none = !std::strcmp(var.value, "none");
    const int mx = none ? 0 : std::min({32, x0, right}), my = none ? 0 : std::min({36, y0, below});
    return {x0 - mx, y0 - my, 320 + 2 * mx, 200 + 2 * my};
}

void notify(const std::string& line) {
    static std::string text;
    text = line;
    retro_message msg{text.c_str(), 600};
    envCb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

// Files go through the frontend's file system when it offers one (VFS: on some platforms
// paths are not plain files), else through the C library. Nothing larger than 64 MB is read.
retro_vfs_interface* vfs = nullptr;
constexpr size_t kMaxFile = 64u << 20;

bool readWhole(const std::string& path, std::vector<uint8_t>& bytes) {
    if (vfs) {
        retro_vfs_file_handle* h = vfs->open(path.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
        if (!h) return false;
        const int64_t size = vfs->size(h);
        bool ok = size >= 0 && static_cast<uint64_t>(size) <= kMaxFile;
        if (ok) {
            bytes.resize(static_cast<size_t>(size));
            ok = size == 0 || vfs->read(h, bytes.data(), static_cast<uint64_t>(size)) == size;
        }
        vfs->close(h);
        return ok;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0 && bytes.size() < kMaxFile) bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(f);
    return true;
}

// Written to "<path>.tmp" first and renamed over `path`, so a crash never leaves half a file.
bool writeWhole(const std::string& path, const std::vector<uint8_t>& data) {
    const std::string tmp = path + ".tmp";
    if (vfs) {
        retro_vfs_file_handle* h = vfs->open(tmp.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
        if (!h) return false;
        const bool ok = vfs->write(h, data.data(), data.size()) == static_cast<int64_t>(data.size());
        if (vfs->close(h) != 0 || !ok) return false;
        return vfs->rename(tmp.c_str(), path.c_str()) == 0;
    }
    FILE* f = std::fopen(tmp.c_str(), "wb");
    const bool ok = f && std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (f && std::fclose(f) != 0) return false;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

void saveDisk(DiskSlot& slot) {
    if (!slot.disk || !slot.disk->changed || slot.savePath.empty()) return;
    if (writeWhole(slot.savePath, k64::writeG64(*slot.disk))) slot.disk->changed = false;
    else log(RETRO_LOG_ERROR, "could not save the written disk to %s", slot.savePath.c_str());
}

void saveDisks() { for (DiskSlot& slot : slots) saveDisk(slot); }

void saveTape() {
    if (!tape || !tape->changed || tapeSavePath.empty() || !machine) return;
    if (writeWhole(tapeSavePath, k64::writeTap(*tape, machine->timing().pal ? 0 : 1))) tape->changed = false;
    else log(RETRO_LOG_ERROR, "could not save the recorded tape to %s", tapeSavePath.c_str());
}

// The save folder's copy for content at `path`: "<save folder>/<name without extension>.g64".
std::string diskCopyPath(const char* path) {
    const char* saveDir = nullptr;
    if (!path || !envCb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDir) || !saveDir) return {};
    std::string name = path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return std::string(saveDir) + "/" + name + ".g64";
}

// The slot's disk: its kept copy when there is one, else the image file.
bool loadSlot(DiskSlot& slot, std::string& error) {
    if (slot.disk) return true;
    auto d = std::make_shared<k64::GcrDisk>();
    std::vector<uint8_t> bytes;
    if (!slot.savePath.empty() && readWhole(slot.savePath, bytes) && k64::parseDisk(bytes.data(), bytes.size(), *d, error)) {
        log(RETRO_LOG_INFO, "disk: the written copy %s", slot.savePath.c_str());
    } else {
        bytes.clear();
        if (!readWhole(slot.path, bytes)) { error = "cannot read " + slot.path; return false; }
        if (!k64::parseDisk(bytes.data(), bytes.size(), *d, error)) return false;
    }
    slot.disk = d;
    return true;
}

void insertSlot() {
    if (slotIndex >= slots.size()) return;
    std::string error;
    if (!loadSlot(slots[slotIndex], error)) {
        log(RETRO_LOG_ERROR, "disk %u: %s", static_cast<unsigned>(slotIndex + 1), error.c_str());
        notify("Kernal64: " + error);
        return;
    }
    disk = slots[slotIndex].disk;
    machine->insertDisk(disk);
}

// An .m3u: one image path per line (relative to the list), # lines are comments.
std::vector<std::string> readM3u(const std::string& path) {
    std::vector<std::string> out;
    std::vector<uint8_t> bytes;
    if (!readWhole(path, bytes)) return out;
    const std::string dir = path.find_last_of("/\\") == std::string::npos ? "" : path.substr(0, path.find_last_of("/\\") + 1);
    std::string line;
    for (size_t i = 0; i <= bytes.size(); ++i) {
        const char c = i < bytes.size() ? static_cast<char>(bytes[i]) : '\n';
        if (c != '\n' && c != '\r') { line += c; continue; }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (!line.empty() && line[0] != '#') out.push_back(line[0] == '/' ? line : dir + line);
        line.clear();
    }
    return out;
}

bool hasExtension(const char* path, const char* ext) {
    if (!path) return false;
    const size_t n = std::strlen(path), m = std::strlen(ext);
    if (n < m) return false;
    for (size_t k = 0; k < m; ++k)
        if (std::tolower(static_cast<unsigned char>(path[n - m + k])) != ext[k]) return false;
    return true;
}

bool isDiskImage(const std::vector<uint8_t>& b) {
    const size_t n = b.size();
    return (n >= 8 && !std::memcmp(b.data(), "GCR-1541", 8)) || (n >= 13 && !std::memcmp(b.data(), "MNIB-1541-RAW", 13)) || n == 174848 || n == 175531 || n == 196608 ||
           n == 197376 || n == 205312 || n == 206114;
}

// A D64 whose directory (track 18 from sector 1 on, the chain the BAM names) holds no file entry.
// False for anything that is not a D64, or whose directory cannot be read: those are started as usual.
bool d64DirectoryEmpty(const std::vector<uint8_t>& b) {
    if (b.size() != 174848 && b.size() != 175531 && b.size() != 196608 && b.size() != 197376 && b.size() != 205312 && b.size() != 206114)
        return false;
    auto offset = [](int track, int sector) {
        size_t o = 0;
        for (int t = 1; t < track; ++t) o += static_cast<size_t>(k64::sectorsOnTrack(t)) * 256;
        return o + static_cast<size_t>(sector) * 256;
    };
    int t = b[offset(18, 0)], s = b[offset(18, 0) + 1];
    for (int hops = 0; t == 18 && hops < 19; ++hops) {
        const size_t o = offset(t, s);
        if (s >= k64::sectorsOnTrack(18) || o + 256 > b.size()) return false;
        for (int e = 0; e < 8; ++e)
            if (b[o + static_cast<size_t>(e) * 32 + 2] != 0) return false;   // a file type: a file
        t = b[o];
        s = b[o + 1];
    }
    return t == 0;
}

// --- disk control (the frontend's eject / pick / insert) ---------------------------------
bool RETRO_CALLCONV dcSetEject(bool ejected) {
    if (!machine || ejected == diskEjected) return true;
    diskEjected = ejected;
    if (ejected) {
        saveDisks();
        machine->ejectDisk();
        disk.reset();
    } else {
        insertSlot();
    }
    return true;
}
bool RETRO_CALLCONV dcGetEject() { return diskEjected; }
unsigned RETRO_CALLCONV dcGetIndex() { return static_cast<unsigned>(slotIndex); }
bool RETRO_CALLCONV dcSetIndex(unsigned i) {
    if (!diskEjected) return false;
    slotIndex = i;   // i == number of slots: no disk
    return true;
}
unsigned RETRO_CALLCONV dcNumImages() { return static_cast<unsigned>(slots.size()); }
bool RETRO_CALLCONV dcReplace(unsigned i, const retro_game_info* info) {
    if (i >= slots.size()) return false;
    saveDisk(slots[i]);
    if (!info) { slots.erase(slots.begin() + i); return true; }
    if (!info->path) return false;
    slots[i] = DiskSlot{info->path, diskCopyPath(info->path), nullptr};
    return true;
}
bool RETRO_CALLCONV dcAdd() { slots.push_back(DiskSlot{}); return true; }
unsigned initialIndex = 0;
std::string initialPath;
bool RETRO_CALLCONV dcSetInitial(unsigned i, const char* path) {
    initialIndex = i;
    initialPath = path ? path : "";
    return true;
}
bool RETRO_CALLCONV dcGetPath(unsigned i, char* out, size_t len) {
    if (i >= slots.size() || slots[i].path.empty() || !len) return false;
    std::snprintf(out, len, "%s", slots[i].path.c_str());
    return true;
}
bool RETRO_CALLCONV dcGetLabel(unsigned i, char* out, size_t len) {
    if (i >= slots.size() || slots[i].path.empty() || !len) return false;
    std::string name = slots[i].path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    std::snprintf(out, len, "%s", name.c_str());
    return true;
}
const retro_disk_control_ext_callback kDiskControl = {dcSetEject, dcGetEject, dcGetIndex, dcSetIndex, dcNumImages,
                                                      dcReplace, dcAdd, dcSetInitial, dcGetPath, dcGetLabel};

bool loadContent(const retro_game_info* info) {
    std::vector<uint8_t> bytes;
    if (info->data && info->size) {
        const auto* p = static_cast<const uint8_t*>(info->data);
        bytes.assign(p, p + info->size);
    } else if (info->path) {
        readWhole(info->path, bytes);
    }
    std::string error;
    if (bytes.empty()) {
        error = "the file could not be read";
    } else if (bytes.size() >= 16 && !std::memcmp(bytes.data() + 4, "CARTRIDGE", 9)) {
        k64::CartImage image;
        if (k64::parseCrt(bytes.data(), bytes.size(), image, error) && machine->attachCartridge(image, error)) {
            log(RETRO_LOG_INFO, "cartridge: hardware type %u, %u chip packet(s)", static_cast<unsigned>(image.type),
                static_cast<unsigned>(image.chips.size()));
            return true;
        }
    } else if (bytes.size() >= 12 && !std::memcmp(bytes.data(), "C64-TAPE-RAW", 12)) {
        // A tape: its kept copy (what the datasette recorded) when there is one. It starts with
        // PLAY down and LOAD + RUN typed, as on the real machine.
        tape = std::make_shared<k64::Tape>();
        tapeSavePath = diskCopyPath(info->path);
        if (!tapeSavePath.empty()) tapeSavePath.replace(tapeSavePath.size() - 4, 4, ".tap");
        std::vector<uint8_t> copy;
        std::string e2;
        const bool haveCopy = !tapeSavePath.empty() && readWhole(tapeSavePath, copy) && k64::parseTap(copy.data(), copy.size(), *tape, e2);
        if (haveCopy || k64::parseTap(bytes.data(), bytes.size(), *tape, error)) {
            if (haveCopy) log(RETRO_LOG_INFO, "tape: the recorded copy %s", tapeSavePath.c_str());
            machine->datasette().insert(tape);
            if (tape->pulses.empty()) {
                // A blank tape (nothing on it, nothing recorded yet): in with RECORD and PLAY down, so a SAVE
                // records at once; LOAD would only search forever.
                machine->datasette().press(k64::Datasette::Button::Record);
                log(RETRO_LOG_INFO, "blank tape in the datasette, RECORD and PLAY down");
                return true;
            }
            machine->datasette().press(k64::Datasette::Button::Play);
            machine->setAutotype("LOAD\rRUN\r");
            log(RETRO_LOG_INFO, "tape in the datasette, started with PLAY, LOAD and RUN");
            return true;
        }
    } else if (isDiskImage(bytes) || hasExtension(info->path, ".m3u")) {
        slots.clear();
        diskEjected = false;
        if (isDiskImage(bytes)) {
            slots.push_back(DiskSlot{info->path ? info->path : "", diskCopyPath(info->path), nullptr});
            // The bytes the frontend handed us, unless a kept copy wins (loadSlot reads that).
            std::vector<uint8_t> copy;
            if (slots[0].savePath.empty() || !readWhole(slots[0].savePath, copy)) {
                auto d = std::make_shared<k64::GcrDisk>();
                if (k64::parseDisk(bytes.data(), bytes.size(), *d, error)) slots[0].disk = d;
            }
        } else {
            for (const std::string& p : readM3u(info->path)) slots.push_back(DiskSlot{p, diskCopyPath(p.c_str()), nullptr});
        }
        slotIndex = (!initialPath.empty() && initialIndex < slots.size() && slots[initialIndex].path == initialPath) ? initialIndex : 0;
        if (slots.empty()) {
            error = "the disk list names no disk";
        } else if (!machine->driveAttached()) {
            error = "a disk needs drive 8: the 1541 ROMs in the system folder and the drive switched on";
        } else if (!loadSlot(slots[slotIndex], error)) {
            // error says why
        } else {
            insertSlot();
            std::vector<uint8_t> kept;
            const bool written = !slots[slotIndex].savePath.empty() && readWhole(slots[slotIndex].savePath, kept);
            if (isDiskImage(bytes) && !written && d64DirectoryEmpty(bytes)) {
                // A disk with no files (a new, formatted one): nothing to start, the READY prompt.
                log(RETRO_LOG_INFO, "empty disk in drive 8");
                return true;
            }
            machine->setAutotype("LOAD\"*\",8,1\rRUN\r");
            log(RETRO_LOG_INFO, "disk in drive 8, started with LOAD\"*\",8,1 and RUN");
            return true;
        }
    } else {
        k64::ProgramFile program;
        if (k64::parseProgram(bytes.data(), bytes.size(), program, error)) {
            machine->setAutostart(program);
            log(RETRO_LOG_INFO, "program: %u bytes at $%04x, started after boot", static_cast<unsigned>(program.data.size()),
                static_cast<unsigned>(program.load));
            return true;
        }
    }
    log(RETRO_LOG_ERROR, "content not loaded: %s", error.c_str());
    notify("Kernal64: " + error);
    return false;
}

void fillAvInfo(retro_system_av_info* info) {
    const auto& t = machine ? machine->timing() : k64::kTimingPal;
    const int w = machine ? crop.w : t.visibleWidth, h = machine ? crop.h : t.visibleLines;
    info->geometry.base_width = static_cast<unsigned>(w);
    info->geometry.base_height = static_cast<unsigned>(h);
    info->geometry.max_width = k64::kMaxRasterWidth;
    info->geometry.max_height = k64::kMaxRasterHeight;
    info->geometry.aspect_ratio = static_cast<float>(w * t.pixelAspect() / h);
    info->timing.fps = t.framesPerSecond();
    info->timing.sample_rate = kSampleRate;
}

} // namespace

extern "C" {

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb) {
    envCb = cb;
    bool noGame = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
    k64lr::registerOptions(cb);
    cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, const_cast<retro_disk_control_ext_callback*>(&kDiskControl));
    // The devices a player can plug in (local-notes/hw/input.md).
    static const retro_controller_description kTypes[] = {
        {"Joystick", RETRO_DEVICE_JOYPAD},
        {"Paddles", kDevicePaddles},
        {"Mouse (1351)", RETRO_DEVICE_MOUSE},
        {"Light pen (control port 1)", RETRO_DEVICE_LIGHTGUN},
        {"None", RETRO_DEVICE_NONE},
    };
    static const retro_controller_info kPorts[] = {{kTypes, 5}, {kTypes, 5}, {nullptr, 0}};
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<retro_controller_info*>(kPorts));
    if (!cb(RETRO_ENVIRONMENT_GET_LED_INTERFACE, &ledCb)) ledCb = {};
    // VFS version 1 has everything used here (open, size, read, write, rename).
    retro_vfs_interface_info vfsInfo{1, nullptr};
    vfs = cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfsInfo) ? vfsInfo.iface : nullptr;
    retro_log_callback logging{};
    logCb = cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) ? logging.log : nullptr;
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { videoCb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audioSampleCb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audioBatchCb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { inputPollCb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { inputStateCb = cb; }

RETRO_API void retro_init(void) {
    frameOut.assign(static_cast<size_t>(k64::kMaxRasterWidth) * k64::kMaxRasterHeight, 0u);
}

RETRO_API void retro_deinit(void) {
    machine.reset();
    frameOut.clear();
    audioOut.clear();
}

RETRO_API void retro_get_system_info(retro_system_info* info) {
    std::memset(info, 0, sizeof *info);
    info->library_name = "Kernal64";
    info->library_version = KERNAL64_VERSION;
    info->valid_extensions = "crt|prg|p00|t64|d64|g64|nib|m3u|tap";
    info->need_fullpath = false;
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info) { fillAvInfo(info); }

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned dev) {
    if (port > 1) return;
    device[port] = dev;
    if (dev == RETRO_DEVICE_LIGHTGUN && portOf(static_cast<int>(port)) != 1)
        notify("Kernal64: a light pen works in control port 1 only");
}

RETRO_API void retro_reset(void) {
    if (machine) machine->reset();
}

RETRO_API bool retro_load_game(const retro_game_info* info) {
    retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!envCb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        log(RETRO_LOG_ERROR, "frontend does not accept XRGB8888");
        return false;
    }
    contentName = info && info->path ? info->path : "";
    audioDue = 0.0;
    audioSent = 0;
    machine = std::make_unique<k64::C64>(modelFromOptions());
    // The memory map for achievements and cheats: the 64 KB of RAM at its own addresses.
    static retro_memory_descriptor ramDesc{};
    ramDesc = {};
    ramDesc.flags = RETRO_MEMDESC_SYSTEM_RAM;
    ramDesc.ptr = machine->ramData();
    ramDesc.len = 65536;
    ramDesc.addrspace = "RAM";
    static retro_memory_map map{};
    map = {&ramDesc, 1};
    envCb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
    readPadOptions();
    setDescriptors();
    osk.setOpen(false);
    portsSwapped = false;
    std::memset(mouseAcc, 0, sizeof mouseAcc);
    std::memset(mousePos, 0, sizeof mousePos);
    machine->setCiaModel(ciaFromOptions());
    machine->setSidModel(sidFromOptions());
    machine->setSidNoisePulseDrains(sidNoisePulseDrainsFromOptions());
    machine->setDriveWanted(driveFromOptions());
    machine->setAudioRate(kSampleRate);
    crop = cropFromOptions();
    log(RETRO_LOG_INFO, "version %s, machine %s, CIA %s", KERNAL64_VERSION, machine->timing().name,
        machine->ciaModel() == k64::CiaModel::Mos8521 ? "8521" : "6526");

    // System ROMs, recognized by content in the frontend's system folder (or a "kernal64"
    // folder inside it).
    const char* sysDir = nullptr;
    if (!envCb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysDir) || !sysDir) sysDir = ".";
    const k64::RomSet roms = k64::findRoms(sysDir);
    std::string missing;
    for (k64::RomRole r : {k64::RomRole::Basic, k64::RomRole::Kernal, k64::RomRole::Chargen}) {
        if (roms.has(r)) log(RETRO_LOG_INFO, "%s: from %s", k64::romName(r), roms.from[static_cast<size_t>(r)].c_str());
        else missing += std::string(missing.empty() ? "" : ", ") + k64::romName(r);
    }
    machine->setRoms(roms);
    osk.setFont(roms.has(k64::RomRole::Chargen) ? roms.image[static_cast<size_t>(k64::RomRole::Chargen)].data() : nullptr);
    if (roms.has(k64::RomRole::Drive1541Low) && roms.has(k64::RomRole::Drive1541High)) {
        for (k64::RomRole r : {k64::RomRole::Drive1541Low, k64::RomRole::Drive1541High})
            log(RETRO_LOG_INFO, "%s: from %s", k64::romName(r), roms.from[static_cast<size_t>(r)].c_str());
    } else if (driveFromOptions()) {
        log(RETRO_LOG_WARN, "no 1541 ROMs in %s: drive 8 is off", sysDir);
        notify("Kernal64: no 1541 ROMs in the system folder: drive 8 is off");
    }
    if (info && (info->data || info->path) && !loadContent(info)) {
        machine.reset();
        return false;
    }
    machine->reset();
    if (!missing.empty()) {
        log(RETRO_LOG_ERROR, "missing system files in %s: %s", sysDir, missing.c_str());
        notify("Kernal64: missing system files: " + missing);
    }
    return true;
}

RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }

RETRO_API void retro_unload_game(void) {
    saveDisks();
    saveTape();
    tape.reset();
    tapeSavePath.clear();
    slots.clear();
    disk.reset();
    machine.reset();
}

RETRO_API unsigned retro_get_region(void) {
    return machine && !machine->timing().pal ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

RETRO_API void retro_run(void) {
    bool updated = false;
    if (envCb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        machine->setCiaModel(ciaFromOptions());
        machine->setSidModel(sidFromOptions());
        machine->setSidNoisePulseDrains(sidNoisePulseDrainsFromOptions());
        machine->setDriveWanted(driveFromOptions());
        readPadOptions();
        setDescriptors();
        const k64::VicModel m = modelFromOptions();
        const bool newModel = m != machine->timing().model;
        if (newModel) machine->setModel(m);
        const Crop c = cropFromOptions();
        const bool newCrop = c.x != crop.x || c.y != crop.y || c.w != crop.w || c.h != crop.h;
        crop = c;
        retro_system_av_info av{};
        fillAvInfo(&av);
        if (newModel) envCb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
        else if (newCrop) envCb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av.geometry);
    }
    if (inputPollCb) inputPollCb();
    applyInput();   // once per frame, at the frame boundary (D9)

    // Fast loading (option kernal64_fast_loading): while the tape moves or the KERNAL pauses after
    // FOUND (or, with disks, the 1541's
    // motor runs) each frame shown is up to kFastFactor frames of the machine, sound muted. A fixed
    // factor decided from the machine's own state, so rewind, run-ahead and netplay stay exact.
    bool fast = false;
    if (machine->hasSystemRoms()) {
        machine->runFrame();
        for (int extra = 1; extra < kFastFactor && loadingNow(); ++extra) {
            std::vector<float> drop(machine->audioAvailable());
            machine->readAudio(drop.data(), drop.size());
            machine->runFrame();
            fast = true;
        }
    }

    const auto& t = machine->timing();
    machine->visibleFrame(frameOut.data());
    osk.draw(frameOut.data() + static_cast<size_t>(crop.y) * static_cast<size_t>(t.visibleWidth) + static_cast<size_t>(crop.x),
             static_cast<size_t>(t.visibleWidth), crop.w, crop.h);
    // The drive's activity light on the frontend's LED 0.
    if (ledCb.set_led_state && machine->driveAttached() && machine->drive().led() != ledWas) {
        ledWas = machine->drive().led();
        ledCb.set_led_state(0, ledWas ? 1 : 0);
    }
    videoCb(frameOut.data() + static_cast<size_t>(crop.y) * static_cast<size_t>(t.visibleWidth) + static_cast<size_t>(crop.x),
            static_cast<unsigned>(crop.w), static_cast<unsigned>(crop.h), static_cast<size_t>(t.visibleWidth) * sizeof(uint32_t));

    // The audio this frame produced: mono, the same on both channels. A fast frame hands over silence,
    // as many samples as keep the total on time (sample rate / frame rate per frame shown).
    size_t frames = machine->audioAvailable();
    audioMono.resize(frames);
    machine->readAudio(audioMono.data(), frames);
    audioDue += kSampleRate / t.framesPerSecond();
    if (fast) {
        frames = audioDue > static_cast<double>(audioSent) ? static_cast<size_t>(audioDue - static_cast<double>(audioSent)) : 0;
        audioMono.assign(frames, 0.0f);
    }
    audioSent += frames;
    audioOut.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const float v = audioMono[i] * 32767.0f;
        const int16_t s = static_cast<int16_t>(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : std::lrintf(v));
        audioOut[2 * i] = audioOut[2 * i + 1] = s;
    }
    size_t done = 0;
    while (done < frames && audioBatchCb) done += audioBatchCb(audioOut.data() + done * 2, frames - done);
    // What the drive wrote is kept once it has stopped writing (motor off).
    if (disk && disk->changed && !machine->drive().motor()) saveDisks();
    // A recording is kept once the datasette's buttons are up again.
    if (tape && tape->changed && machine->datasette().button() == k64::Datasette::Button::None) saveTape();
}

// A state = the machine's state, which holds the surface of the disk in the drive, then a tail
// (little-endian): which disk of the list that is (4 bytes; 0xFFFFFFFF = the drive is empty),
// whether the joystick ports are swapped (1), the 1351 mice's positions and movement still to
// pass on (8 x 4), the RetroPad buttons as last seen (2 x 16), the on-screen keyboard's owner
// (1) and state: what a replay needs to come out the same.
constexpr size_t kDiskTailSize = 4 + 1 + 8 * 4 + 32 + 1 + k64lr::Osk::kStateSize;
void putTail(uint8_t* out, uint32_t inDrive) {
    auto put32 = [&out](uint32_t v) { for (int b = 0; b < 4; ++b) *out++ = static_cast<uint8_t>(v >> (8 * b)); };
    put32(inDrive);
    *out++ = portsSwapped ? 1 : 0;
    for (int p = 0; p < 2; ++p)
        for (int a = 0; a < 2; ++a) { put32(static_cast<uint32_t>(mousePos[p][a])); put32(static_cast<uint32_t>(mouseAcc[p][a])); }
    for (const auto& player : padWas)
        for (bool b : player) *out++ = b ? 1 : 0;
    *out++ = static_cast<uint8_t>(oskOwner);
    osk.saveState(out);
}
void getTailInput(const uint8_t* in) {
    auto get32 = [&in]() { const uint32_t v = static_cast<uint32_t>(in[0] | in[1] << 8 | in[2] << 16 | static_cast<uint32_t>(in[3]) << 24); in += 4; return v; };
    in += 4;
    portsSwapped = *in++ != 0;
    for (int p = 0; p < 2; ++p)
        for (int a = 0; a < 2; ++a) { mousePos[p][a] = static_cast<int>(get32()); mouseAcc[p][a] = static_cast<int>(get32()); }
    for (auto& player : padWas)
        for (bool& b : player) b = *in++ != 0;
    oskOwner = *in++ & 1;
    osk.loadState(in);
}
constexpr uint32_t kNoDisk = 0xFFFFFFFFu;

RETRO_API size_t retro_serialize_size(void) { return machine ? kDiskTailSize + machine->stateSize() : 0; }

RETRO_API bool retro_serialize(void* data, size_t size) {
    if (!machine || size < retro_serialize_size()) return false;
    auto* out = static_cast<uint8_t*>(data);
    const uint32_t inDrive = disk ? static_cast<uint32_t>(slotIndex) : kNoDisk;
    machine->saveState(out);
    out += machine->stateSize();
    putTail(out, inDrive);
    return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
    if (!machine || size < retro_serialize_size()) return false;
    const auto* in = static_cast<const uint8_t*>(data);
    const uint8_t* tail = in + machine->stateSize();
    const uint32_t inDrive = static_cast<uint32_t>(tail[0] | tail[1] << 8 | tail[2] << 16 | static_cast<uint32_t>(tail[3]) << 24);
    if (inDrive != kNoDisk && inDrive >= slots.size()) return false;   // a state of other content
    if (!machine->stateFits(in, machine->stateSize())) return false;   // refused before anything changes
    // The state's disk goes into the drive first (the machine state then restores its surface
    // and the drive's own view of the swap).
    const bool same = inDrive == kNoDisk ? !disk : (disk && slotIndex == inDrive);
    if (!same) {
        std::string error;
        if (inDrive != kNoDisk && !loadSlot(slots[inDrive], error)) return false;
        saveDisks();
        if (inDrive == kNoDisk) {
            machine->ejectDisk();
            disk.reset();
            diskEjected = true;
        } else {
            slotIndex = inDrive;
            disk = slots[inDrive].disk;
            machine->insertDisk(disk);
            diskEjected = false;
        }
    }
    if (!machine->loadState(in, machine->stateSize())) return false;
    getTailInput(tail);
    return true;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}
RETRO_API void* retro_get_memory_data(unsigned id) {
    return machine && id == RETRO_MEMORY_SYSTEM_RAM ? machine->ramData() : nullptr;
}
RETRO_API size_t retro_get_memory_size(unsigned id) {
    return machine && id == RETRO_MEMORY_SYSTEM_RAM ? 65536 : 0;
}

} // extern "C"
