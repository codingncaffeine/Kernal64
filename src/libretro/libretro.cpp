// libretro entry points.
#include "../core/c64.h"
#include "../core/roms.h"
#include "libretro.h"

#include <algorithm>
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

std::vector<uint32_t> frameOut;
std::vector<int16_t> audioOut;
std::vector<float> audioMono;

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
    // The button on a freezer cartridge: Page Down, or a click of the right stick. It is
    // the cartridge's own button, so without such a cartridge it does nothing.
    machine->setFreezeButton(inputStateCb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_PAGEDOWN) != 0 || pad(RETRO_DEVICE_ID_JOYPAD_R3));
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

// The content: a cartridge file, or a single-file program that is put into memory and
// started once the machine has booted. Told apart by what is in it, not by its name.
bool readWhole(const std::string& path, std::vector<uint8_t>& bytes) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(f);
    return true;
}

void saveDisk(DiskSlot& slot) {
    if (!slot.disk || !slot.disk->changed || slot.savePath.empty()) return;
    const auto g = k64::writeG64(*slot.disk);
    const std::string tmp = slot.savePath + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    const bool ok = f && std::fwrite(g.data(), 1, g.size(), f) == g.size();
    if (f) std::fclose(f);
    if (ok && std::rename(tmp.c_str(), slot.savePath.c_str()) == 0) slot.disk->changed = false;
    else log(RETRO_LOG_ERROR, "could not save the written disk to %s", slot.savePath.c_str());
}

void saveDisks() { for (DiskSlot& slot : slots) saveDisk(slot); }

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
    return (n >= 8 && !std::memcmp(b.data(), "GCR-1541", 8)) || n == 174848 || n == 175531 || n == 196608 ||
           n == 197376 || n == 205312 || n == 206114;
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
        FILE* f = std::fopen(info->path, "rb");
        if (f) {
            uint8_t chunk[65536];
            size_t n;
            while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0 && bytes.size() < (64u << 20)) bytes.insert(bytes.end(), chunk, chunk + n);
            std::fclose(f);
        }
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
    static const retro_variable vars[] = {
        {"kernal64_model", "Video chip; pal|pal-new|pal-old|ntsc|ntsc-new|ntsc-old"},
        {"kernal64_cia", "CIA chips; 6526|8521"},
        {"kernal64_sid", "SID chip; 6581|8580"},
        {"kernal64_sid_noise_pulse", "6581: noise + pulse on the noise register; drains it|leaves it alone"},
        {"kernal64_border", "Border; full|small|none"},
        {"kernal64_drive8", "Drive 8 (1541); on|off"},
        {"kernal64_joyport", "RetroPad is the joystick in control port; 2|1"},
        {nullptr, nullptr},
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(vars));
    cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, const_cast<retro_disk_control_ext_callback*>(&kDiskControl));
    // What the RetroPad's buttons are, for the frontend's remapping screen.
    static const retro_input_descriptor inputs[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick right"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Fire"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Cartridge freeze button"},
        {0, 0, 0, 0, nullptr},
    };
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, const_cast<retro_input_descriptor*>(inputs));
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
    info->valid_extensions = "crt|prg|p00|t64|d64|g64|m3u";
    info->need_fullpath = false;
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info) { fillAvInfo(info); }

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API void retro_reset(void) {
    if (machine) machine->reset();
}

RETRO_API bool retro_load_game(const retro_game_info* info) {
    retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!envCb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        log(RETRO_LOG_ERROR, "frontend does not accept XRGB8888");
        return false;
    }
    machine = std::make_unique<k64::C64>(modelFromOptions());
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

    // The audio this frame produced: mono, the same on both channels.
    const size_t frames = machine->audioAvailable();
    audioMono.resize(frames);
    machine->readAudio(audioMono.data(), frames);
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
}

RETRO_API size_t retro_serialize_size(void) { return machine ? machine->stateSize() : 0; }

RETRO_API bool retro_serialize(void* data, size_t size) {
    if (!machine || size < retro_serialize_size()) return false;
    auto* out = static_cast<uint8_t*>(data);
    machine->saveState(out);
    return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
    if (!machine || size < retro_serialize_size()) return false;
    const auto* in = static_cast<const uint8_t*>(data);
    return machine->loadState(in, machine->stateSize());
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
