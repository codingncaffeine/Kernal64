// libretro entry points.
#include "../core/c64.h"
#include "../core/roms.h"
#include "libretro.h"

#include <algorithm>
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

std::unique_ptr<k64::C64> machine;
std::vector<uint32_t> frameOut;
std::vector<int16_t> audioOut;
double audioCarry = 0.0;

constexpr unsigned kSampleRate = 48000;

// The part of the visible picture that is shown (core option kernal64_border).
struct Crop { int x = 0, y = 0, w = 0, h = 0; };
Crop crop;

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

int joystickPort() {
    retro_variable var{"kernal64_joyport", nullptr};
    if (envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !std::strcmp(var.value, "1")) return 1;
    return 2;
}

void applyInput() {
    if (!inputStateCb) return;
    machine->releaseAllKeys();
    for (const auto& k : kKeys) {
        if (!inputStateCb(0, RETRO_DEVICE_KEYBOARD, 0, k.key)) continue;
        machine->setKey(k.col, k.row, true);
        if (k.shift) machine->setKey(1, 7, true);
    }
    machine->setRestore(inputStateCb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_PAGEUP) != 0);
    auto pad = [](unsigned id) { return inputStateCb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0; };
    const uint8_t lines = static_cast<uint8_t>((pad(RETRO_DEVICE_ID_JOYPAD_UP) ? 0x01 : 0) | (pad(RETRO_DEVICE_ID_JOYPAD_DOWN) ? 0x02 : 0) |
                                               (pad(RETRO_DEVICE_ID_JOYPAD_LEFT) ? 0x04 : 0) | (pad(RETRO_DEVICE_ID_JOYPAD_RIGHT) ? 0x08 : 0) |
                                               (pad(RETRO_DEVICE_ID_JOYPAD_B) ? 0x10 : 0));
    const int port = joystickPort();
    machine->setJoystick(port, lines);
    machine->setJoystick(3 - port, 0);
}

k64::VicModel modelFromOptions() {
    retro_variable var{"kernal64_model", nullptr};
    k64::VicModel m = k64::VicModel::Mos6569;
    if (envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) k64::vicModelFromName(var.value, m);
    return m;
}

k64::CiaModel ciaFromOptions() {
    retro_variable var{"kernal64_cia", nullptr};
    if (envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !std::strcmp(var.value, "8521"))
        return k64::CiaModel::Mos8521;
    return k64::CiaModel::Mos6526;
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
    static const retro_variable vars[] = {
        {"kernal64_model", "Video chip; pal|pal-new|pal-old|ntsc|ntsc-new|ntsc-old"},
        {"kernal64_cia", "CIA chips; 6526|8521"},
        {"kernal64_border", "Border; full|small|none"},
        {"kernal64_joyport", "RetroPad is the joystick in control port; 2|1"},
        {nullptr, nullptr},
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(vars));
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
    info->valid_extensions = "prg|t64|d64|g64|tap|crt|m3u";
    info->need_fullpath = true;
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info) { fillAvInfo(info); }

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API void retro_reset(void) {
    if (machine) machine->reset();
    audioCarry = 0.0;
}

RETRO_API bool retro_load_game(const retro_game_info*) {
    retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!envCb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        log(RETRO_LOG_ERROR, "frontend does not accept XRGB8888");
        return false;
    }
    machine = std::make_unique<k64::C64>(modelFromOptions());
    machine->setCiaModel(ciaFromOptions());
    crop = cropFromOptions();
    audioCarry = 0.0;
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
    machine->reset();
    if (!missing.empty()) {
        log(RETRO_LOG_ERROR, "missing system files in %s: %s", sysDir, missing.c_str());
        static std::string text;
        text = "Kernal64: missing system files: " + missing;
        retro_message msg{text.c_str(), 600};
        envCb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    }
    return true;
}

RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }

RETRO_API void retro_unload_game(void) { machine.reset(); }

RETRO_API unsigned retro_get_region(void) {
    return machine && !machine->timing().pal ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

RETRO_API void retro_run(void) {
    bool updated = false;
    if (envCb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        machine->setCiaModel(ciaFromOptions());
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

    if (machine->hasSystemRoms()) machine->runFrame();

    const auto& t = machine->timing();
    machine->visibleFrame(frameOut.data());
    videoCb(frameOut.data() + static_cast<size_t>(crop.y) * static_cast<size_t>(t.visibleWidth) + static_cast<size_t>(crop.x),
            static_cast<unsigned>(crop.w), static_cast<unsigned>(crop.h), static_cast<size_t>(t.visibleWidth) * sizeof(uint32_t));

    // One frame of audio; the fraction of a sample left over is carried to the next frame.
    audioCarry += kSampleRate / machine->timing().framesPerSecond();
    const size_t frames = static_cast<size_t>(audioCarry);
    audioCarry -= static_cast<double>(frames);
    audioOut.assign(frames * 2, 0);
    size_t done = 0;
    while (done < frames && audioBatchCb) done += audioBatchCb(audioOut.data() + done * 2, frames - done);
}

RETRO_API size_t retro_serialize_size(void) { return machine ? machine->stateSize() + sizeof(double) : 0; }

RETRO_API bool retro_serialize(void* data, size_t size) {
    if (!machine || size < retro_serialize_size()) return false;
    auto* out = static_cast<uint8_t*>(data);
    machine->saveState(out);
    std::memcpy(out + machine->stateSize(), &audioCarry, sizeof(double));
    return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
    if (!machine || size < retro_serialize_size()) return false;
    const auto* in = static_cast<const uint8_t*>(data);
    if (!machine->loadState(in, machine->stateSize())) return false;
    std::memcpy(&audioCarry, in + machine->stateSize(), sizeof(double));
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
