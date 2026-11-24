// k64host: a minimal libretro frontend that loads a built core and checks it
// through the API only, the way a real frontend does.
//
//   k64host CORE.so [--content FILE] [--frames N] [--option KEY=VALUE]...
//                   [--system DIR] [--save DIR] [--dump FILE.ppm] [--check]
//                   [--type TEXT] [--type-at FRAME] [--expect-screen TEXT]
//                   [--expect-tone HZ DB_MIN DB_MAX] [--expect-level DB_MIN DB_MAX] [--expect-silence]
//                   [--expect-load-failure] [--freeze-frames N,N,...]
//
// --swap-at FRAME INDEX ejects the disk, picks image INDEX (from 0) and inserts it through the
// core's disk control, as a frontend's disk menu does (not with --check).
// --freeze-frames holds the RetroPad button the core uses as a cartridge's freeze button
// (a click of the right stick) during each frame N and the one after it.
// --type presses the keys for TEXT one at a time from frame --type-at (default 200),
// each held 3 frames and released 3 ("\n" = RETURN; " # $ ? ( ) are typed with SHIFT). --expect-screen fails the run
// unless TEXT is in the C64 text screen ($0400), read through RETRO_MEMORY_SYSTEM_RAM.
// --expect-tone fails the run unless the last half second of audio is a tone of HZ (within
// 0.1 %) at a level between DB_MIN and DB_MAX (RMS, dB below full scale); --expect-level
// checks the level alone; --expect-silence fails unless every sample of the run is zero. The audio is only ever looked at as numbers.
// --expect-load-failure: the core must refuse the content (the run ends there).
// --check runs the conformance checks and exits non-zero on the first failure. Among them:
// a state saved half-way must replay to the same frames and the same end state, in the
// running core and in one that was shut down and started again with the same content.
// Without it the run prints a summary only.
//
// Options: the core's declarations are checked (unique keys, each default among its values,
// every --option value among the values declared for it) and reading an option the core never
// declared fails the run. --options-version N answers the core's version query with N (0 = no
// answer: the plain-variables fallback).
// Input: --device PORT NAME plugs a device into a player's port (joypad, paddles, mouse,
// lightgun, none); --hold FROM TO PORT INPUT VALUE holds an input at VALUE during frames FROM
// to TO: INPUT = a RetroPad button (b a y x up down left right select start l r l2 r2 l3 r3),
// an analog axis (lx ly rx ry), the mouse (mx my = movement per frame, mleft mright) or the
// light gun (gx gy = position -32767..32767, gtrigger, goffscreen).
// --expect-led N: the core's LED 0 must have been switched on at least N times.
#include "libretro.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace {

struct Core {
    void* handle = nullptr;
#define SYM(name) decltype(&::name) name = nullptr;
    SYM(retro_api_version) SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample)
    SYM(retro_set_audio_sample_batch) SYM(retro_set_input_poll) SYM(retro_set_input_state) SYM(retro_init)
    SYM(retro_deinit) SYM(retro_get_system_info) SYM(retro_get_system_av_info) SYM(retro_load_game)
    SYM(retro_unload_game) SYM(retro_run) SYM(retro_reset) SYM(retro_serialize_size) SYM(retro_serialize)
    SYM(retro_unserialize) SYM(retro_get_memory_data) SYM(retro_get_memory_size) SYM(retro_set_controller_port_device)
#undef SYM
};

std::map<std::string, std::string> options;
std::string systemDir = ".", saveDir = ".";
bool optionsChanged = false;
unsigned optionsVersion = 2;
std::map<std::string, std::vector<std::string>> declared;   // option key -> its values
unsigned ledOnCount = 0;
int ledNow = 0;
void ledSet(int led, int state) {
    if (led == 0 && state && !ledNow) ++ledOnCount;
    if (led == 0) ledNow = state;
}
retro_led_interface ledInterface{ledSet};
struct Hold { unsigned from, to, port, device, index, id; int value; };
std::vector<Hold> holds;
std::vector<std::pair<unsigned, unsigned>> devices;   // player port, device
retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
bool pixelFormatSet = false;
retro_system_av_info av{};

struct Frame {
    unsigned width = 0, height = 0;
    size_t pitch = 0;
    std::vector<uint32_t> pixels;
} lastFrame;
unsigned framesSeen = 0;
uint64_t audioFrames = 0, audioFramesFirst = 0;
int failures = 0;

void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "FAIL: ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    ++failures;
}

void coreLog(retro_log_level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// The core's disk control, as a frontend's disk menu uses it (--swap-at).
const retro_disk_control_ext_callback* diskControl = nullptr;
std::vector<std::pair<unsigned, unsigned>> swaps;   // frame, image index

void fail(const char* fmt, ...);

template <typename Def> void declare(const Def* defs) {
    for (const Def* d = defs; d->key; ++d) {
        if (declared.count(d->key)) fail("option %s declared twice", d->key);
        auto& vals = declared[d->key];
        for (const retro_core_option_value* v = d->values; v->value; ++v) vals.push_back(v->value);
        bool found = false;
        for (const auto& v : vals) found |= d->default_value && v == d->default_value;
        if (!found) fail("option %s: default %s is not among its values", d->key, d->default_value ? d->default_value : "(none)");
    }
}

bool environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        if (!optionsVersion) return false;
        *static_cast<unsigned*>(data) = optionsVersion;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        if (optionsVersion < 2) return false;
        declared.clear();
        declare(static_cast<const retro_core_options_v2*>(data)->definitions);
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        if (optionsVersion < 1) return false;
        declared.clear();
        declare(static_cast<const retro_core_option_definition*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        declared.clear();
        for (const auto* v = static_cast<const retro_variable*>(data); v->key; ++v) {
            const std::string text = v->value ? v->value : "";
            const size_t semi = text.find("; ");
            if (semi == std::string::npos) { fail("variable %s has no \"; \" in %s", v->key, text.c_str()); continue; }
            auto& vals = declared[v->key];
            for (size_t p = semi + 2; p <= text.size();) {
                const size_t bar = std::min(text.find('|', p), text.size());
                vals.push_back(text.substr(p, bar - p));
                p = bar + 1;
            }
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;
    case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
        *static_cast<retro_led_interface*>(data) = ledInterface;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        diskControl = static_cast<const retro_disk_control_ext_callback*>(data);
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = coreLog;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        pixelFormat = *static_cast<retro_pixel_format*>(data);
        pixelFormatSet = true;
        return pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888 || pixelFormat == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char**>(data) = systemDir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = saveDir.c_str();
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* v = static_cast<retro_variable*>(data);
        if (!declared.count(v->key)) fail("option %s read but never declared", v->key);
        auto it = options.find(v->key);
        v->value = it == options.end() ? nullptr : it->second.c_str();
        return it != options.end();
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = optionsChanged;
        optionsChanged = false;
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        av = *static_cast<const retro_system_av_info*>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        av.geometry = *static_cast<const retro_game_geometry*>(data);
        return true;
    default:
        return false;
    }
}

void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
    ++framesSeen;
    if (!data) return; // a duplicate frame
    lastFrame.width = width;
    lastFrame.height = height;
    lastFrame.pitch = pitch;
    lastFrame.pixels.resize(static_cast<size_t>(width) * height);
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(&lastFrame.pixels[static_cast<size_t>(y) * width], static_cast<const uint8_t*>(data) + y * pitch,
                    width * sizeof(uint32_t));
}

std::vector<int16_t> audioLeft;   // everything the core sent, left channel
bool audioChannelsDiffer = false;
void audioSample(int16_t l, int16_t r) {
    ++audioFrames;
    audioLeft.push_back(l);
    if (l != r) audioChannelsDiffer = true;
}
size_t audioBatch(const int16_t* data, size_t frames) {
    audioFrames += frames;
    for (size_t i = 0; i < frames; ++i) {
        audioLeft.push_back(data[2 * i]);
        if (data[2 * i] != data[2 * i + 1]) audioChannelsDiffer = true;
    }
    return frames;
}
// Frequency (from the first to the last upward crossing of the mean, each placed between
// its two samples) and level (RMS around the mean, in dB below full scale) of the last half
// second of audio.
struct Tone { double hz, db; };
Tone measureTone(double sampleRate) {
    const size_t n = static_cast<size_t>(sampleRate / 2);
    if (audioLeft.size() < n + 1) return {0.0, -200.0};
    const int16_t* x = audioLeft.data() + (audioLeft.size() - n);
    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) mean += x[i];
    mean /= static_cast<double>(n);
    double sq = 0.0, first = 0.0, last = 0.0;
    int crossings = 0;
    for (size_t i = 0; i < n; ++i) {
        const double a = i > 0 ? x[i - 1] - mean : 0.0, b = x[i] - mean;
        sq += b * b;
        if (i > 0 && a < 0.0 && b >= 0.0) {
            last = static_cast<double>(i - 1) + a / (a - b);
            if (crossings++ == 0) first = last;
        }
    }
    const double rms = std::sqrt(sq / static_cast<double>(n)) / 32768.0;
    const double hz = crossings > 1 ? (crossings - 1) / ((last - first) / sampleRate) : 0.0;
    return {hz, rms > 0.0 ? 20.0 * std::log10(rms) : -200.0};
}
// Scripted typing: the key held during the current frame (or none).
// Each --type is typed from the --type-at given before it (default 200).
struct Typing { unsigned at; std::string text; };
std::vector<Typing> typings;
unsigned typeAt = 200, currentFrame = 0;
unsigned keyFor(char ch) {
    if (ch >= 'A' && ch <= 'Z') return RETROK_a + static_cast<unsigned>(ch - 'A');
    if (ch >= 'a' && ch <= 'z') return RETROK_a + static_cast<unsigned>(ch - 'a');
    if (ch >= '0' && ch <= '9') return RETROK_0 + static_cast<unsigned>(ch - '0');
    switch (ch) {
    case ' ': return RETROK_SPACE;
    case '\n': return RETROK_RETURN;
    case '*': return RETROK_RIGHTBRACKET;   // the core's positional map: ] = *
    case '+': return RETROK_MINUS;          // - = +
    case '-': return RETROK_EQUALS;         // = = -
    case '.': return RETROK_PERIOD;
    case ',': return RETROK_COMMA;
    case ':': return RETROK_SEMICOLON;      // ; = :
    case '/': return RETROK_SLASH;
    case ';': return RETROK_QUOTE;          // ' = ;
    case '=': return RETROK_BACKSLASH;      // \ = =
    // With SHIFT (shifted() below): the C64's own shifted keys.
    case '"': return RETROK_2;
    case '#': return RETROK_3;
    case '$': return RETROK_4;
    case '?': return RETROK_SLASH;
    case '(': return RETROK_8;
    case ')': return RETROK_9;
    default: return RETROK_UNKNOWN;
    }
}
bool shifted(char ch) { return ch == '"' || ch == '#' || ch == '$' || ch == '?' || ch == '(' || ch == ')'; }
std::vector<unsigned> freezeFrames;
void inputPoll() {}
int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    for (const Hold& h : holds)
        if (currentFrame >= h.from && currentFrame <= h.to && h.port == port && h.device == device && h.index == index && h.id == id)
            return static_cast<int16_t>(h.value);
    if (port == 0 && device == RETRO_DEVICE_JOYPAD && id == RETRO_DEVICE_ID_JOYPAD_R3) {
        for (unsigned f : freezeFrames)
            if (currentFrame == f || currentFrame == f + 1) return 1;
        return 0;
    }
    if (port != 0 || device != RETRO_DEVICE_KEYBOARD) return 0;
    for (const Typing& t : typings) {
        if (currentFrame < t.at) continue;
        const unsigned step = (currentFrame - t.at) / 6, phase = (currentFrame - t.at) % 6;
        if (step >= t.text.size() || phase >= 3) continue;
        if (id == RETROK_LSHIFT && shifted(t.text[step])) return 1;
        if (keyFor(t.text[step]) == id) return 1;
    }
    return 0;
}

uint64_t fnv1a(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
    return h;
}

bool writePpm(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", lastFrame.width, lastFrame.height);
    for (uint32_t px : lastFrame.pixels) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(px >> 16), static_cast<unsigned char>(px >> 8),
                                      static_cast<unsigned char>(px)};
        std::fwrite(rgb, 1, 3, f);
    }
    return std::fclose(f) == 0;
}

template <class T> bool load(void* h, const char* name, T& out) {
    out = reinterpret_cast<T>(dlsym(h, name));
    if (!out) std::fprintf(stderr, "missing symbol %s\n", name);
    return out != nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: k64host CORE.so [--content FILE] [--frames N] [--option K=V] [--system DIR] "
                             "[--save DIR] [--dump FILE.ppm] [--check]\n");
        return 2;
    }
    std::string content, dump;
    unsigned frames = 60;
    bool check = false;
    std::vector<std::string> expectScreen;
    double expectHz = 0.0, expectDbMin = 0.0, expectDbMax = 0.0;
    bool expectSilence = false, expectLevel = false, expectLoadFailure = false;
    unsigned expectLed = 0;
    double levelMin = 0.0, levelMax = 0.0;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--content") content = next();
        else if (a == "--frames") frames = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--system") systemDir = next();
        else if (a == "--save") saveDir = next();
        else if (a == "--dump") dump = next();
        else if (a == "--check") check = true;
        else if (a == "--type") {
            std::string text = next();
            for (size_t p = 0; (p = text.find("\\n", p)) != std::string::npos;) text.replace(p, 2, "\n");
            typings.push_back({typeAt, text});
        } else if (a == "--type-at") typeAt = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--expect-screen") expectScreen.push_back(next());
        else if (a == "--swap-at") { const unsigned at = static_cast<unsigned>(std::stoul(next())); swaps.emplace_back(at, static_cast<unsigned>(std::stoul(next()))); }
        else if (a == "--expect-tone") { expectHz = std::stod(next()); expectDbMin = std::stod(next()); expectDbMax = std::stod(next()); }
        else if (a == "--expect-silence") expectSilence = true;
        else if (a == "--expect-load-failure") expectLoadFailure = true;
        else if (a == "--freeze-frames") {
            const std::string list = next();
            for (size_t p = 0; p < list.size();) {
                const size_t comma = list.find(',', p);
                freezeFrames.push_back(static_cast<unsigned>(std::stoul(list.substr(p, comma - p))));
                p = comma == std::string::npos ? list.size() : comma + 1;
            }
        }
        else if (a == "--expect-level") { expectLevel = true; levelMin = std::stod(next()); levelMax = std::stod(next()); }
        else if (a == "--options-version") optionsVersion = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--expect-led") expectLed = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--device") {
            const unsigned port = static_cast<unsigned>(std::stoul(next()));
            const std::string n = next();
            const unsigned d = n == "joypad" ? RETRO_DEVICE_JOYPAD : n == "paddles" ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0)
                             : n == "mouse" ? RETRO_DEVICE_MOUSE : n == "lightgun" ? RETRO_DEVICE_LIGHTGUN : RETRO_DEVICE_NONE;
            devices.emplace_back(port, d);
        } else if (a == "--hold") {
            Hold h{};
            h.from = static_cast<unsigned>(std::stoul(next()));
            h.to = static_cast<unsigned>(std::stoul(next()));
            h.port = static_cast<unsigned>(std::stoul(next()));
            const std::string in = next();
            h.value = std::stoi(next());
            static const std::map<std::string, std::array<unsigned, 3>> kInputs = {
                {"b", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B}}, {"a", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A}},
                {"y", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y}}, {"x", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X}},
                {"up", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP}}, {"down", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN}},
                {"left", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT}}, {"right", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT}},
                {"select", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT}}, {"start", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START}},
                {"l", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L}}, {"r", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R}},
                {"l2", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2}}, {"r2", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2}},
                {"l3", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3}}, {"r3", {RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3}},
                {"lx", {RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X}},
                {"ly", {RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y}},
                {"rx", {RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X}},
                {"ry", {RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y}},
                {"mx", {RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X}}, {"my", {RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y}},
                {"mleft", {RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT}}, {"mright", {RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT}},
                {"gx", {RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X}}, {"gy", {RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y}},
                {"gtrigger", {RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER}},
                {"goffscreen", {RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN}},
            };
            auto it = kInputs.find(in);
            if (it == kInputs.end()) { std::fprintf(stderr, "--hold: unknown input %s\n", in.c_str()); return 2; }
            h.device = it->second[0];
            h.index = it->second[1];
            h.id = it->second[2];
            holds.push_back(h);
        } else if (a == "--option") {
            std::string kv = next();
            auto eq = kv.find('=');
            if (eq == std::string::npos) { std::fprintf(stderr, "--option needs KEY=VALUE\n"); return 2; }
            options[kv.substr(0, eq)] = kv.substr(eq + 1);
        } else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }

    Core c;
    c.handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!c.handle) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    bool ok = true;
#define LOAD(name) ok &= load(c.handle, #name, c.name);
    LOAD(retro_api_version) LOAD(retro_set_environment) LOAD(retro_set_video_refresh) LOAD(retro_set_audio_sample)
    LOAD(retro_set_audio_sample_batch) LOAD(retro_set_input_poll) LOAD(retro_set_input_state) LOAD(retro_init)
    LOAD(retro_deinit) LOAD(retro_get_system_info) LOAD(retro_get_system_av_info) LOAD(retro_load_game)
    LOAD(retro_unload_game) LOAD(retro_run) LOAD(retro_reset) LOAD(retro_serialize_size) LOAD(retro_serialize)
    LOAD(retro_unserialize) LOAD(retro_get_memory_data) LOAD(retro_get_memory_size) LOAD(retro_set_controller_port_device)
#undef LOAD
    if (!ok) return 3;

    if (c.retro_api_version() != RETRO_API_VERSION) fail("API version %u", c.retro_api_version());
    c.retro_set_environment(environment);
    c.retro_set_video_refresh(videoRefresh);
    c.retro_set_audio_sample(audioSample);
    c.retro_set_audio_sample_batch(audioBatch);
    c.retro_set_input_poll(inputPoll);
    c.retro_set_input_state(inputState);
    c.retro_init();

    retro_system_info si{};
    c.retro_get_system_info(&si);
    std::printf("core: %s %s (extensions %s)\n", si.library_name, si.library_version, si.valid_extensions);

    // As a frontend does for a core that does not ask for the path alone: the file's bytes.
    retro_game_info gi{};
    std::vector<uint8_t> contentBytes;
    gi.path = content.empty() ? nullptr : content.c_str();
    if (!content.empty() && !si.need_fullpath) {
        FILE* f = std::fopen(content.c_str(), "rb");
        if (!f) { std::fprintf(stderr, "k64host: cannot open %s\n", content.c_str()); return 3; }
        int ch;
        while ((ch = std::fgetc(f)) != EOF) contentBytes.push_back(static_cast<uint8_t>(ch));
        std::fclose(f);
        gi.data = contentBytes.data();
        gi.size = contentBytes.size();
    }
    const bool loaded = c.retro_load_game(content.empty() ? nullptr : &gi);
    if (expectLoadFailure) {
        if (loaded) { fail("retro_load_game accepted content it should refuse"); return 1; }
        std::printf("content refused, as expected\n");
        c.retro_deinit();
        return 0;
    }
    if (!loaded) { fail("retro_load_game returned false"); return 1; }
    for (const auto& [key, value] : options) {
        auto it = declared.find(key);
        if (it == declared.end()) fail("--option %s: the core declares no such option", key.c_str());
        else if (std::find(it->second.begin(), it->second.end(), value) == it->second.end())
            fail("--option %s=%s: not one of the option's values", key.c_str(), value.c_str());
    }
    for (const auto& [port, dev] : devices) c.retro_set_controller_port_device(port, dev);
    c.retro_get_system_av_info(&av);
    if (check && !pixelFormatSet) fail("core never set a pixel format");
    std::printf("av: %ux%u (max %ux%u) aspect %.4f fps %.4f rate %.0f\n", av.geometry.base_width,
                av.geometry.base_height, av.geometry.max_width, av.geometry.max_height, av.geometry.aspect_ratio,
                av.timing.fps, av.timing.sample_rate);

    std::vector<uint8_t> stateAtHalf;
    std::vector<uint64_t> hashes;
    const size_t stateSize = c.retro_serialize_size();
    for (unsigned f = 0; f < frames; ++f) {
        currentFrame = f;
        for (const auto& [at, index] : swaps) {
            if (at != f) continue;
            const bool swapped = diskControl && diskControl->set_eject_state(true) && diskControl->set_image_index(index) &&
                            diskControl->set_eject_state(false);
            std::printf("disk control: image %u of %u at frame %u%s\n", index + 1,
                        diskControl ? diskControl->get_num_images() : 0, f, swapped ? "" : " FAILED");
        }
        c.retro_run();
        if (f == 0) audioFramesFirst = audioFrames;
        hashes.push_back(fnv1a(lastFrame.pixels.data(), lastFrame.pixels.size() * 4));
        if (f + 1 == frames / 2) {
            stateAtHalf.resize(stateSize);
            if (!c.retro_serialize(stateAtHalf.data(), stateSize)) fail("retro_serialize failed");
        }
        if (check && c.retro_serialize_size() != stateSize) fail("state size changed at frame %u", f);
    }
    std::vector<uint8_t> stateAtEnd(stateSize);
    if (!c.retro_serialize(stateAtEnd.data(), stateSize)) fail("retro_serialize failed");
    std::printf("ran %u frames, last frame %ux%u pitch %zu, hash %016llx, audio %llu frames\n", framesSeen,
                lastFrame.width, lastFrame.height, lastFrame.pitch, static_cast<unsigned long long>(hashes.back()),
                static_cast<unsigned long long>(audioFrames));

    if (check) {
        if (framesSeen != frames) fail("video callback ran %u times for %u frames", framesSeen, frames);
        if (lastFrame.width != av.geometry.base_width || lastFrame.height != av.geometry.base_height)
            fail("frame %ux%u differs from the reported geometry", lastFrame.width, lastFrame.height);
        if (lastFrame.width > av.geometry.max_width || lastFrame.height > av.geometry.max_height)
            fail("frame larger than the reported maximum");
        // Audio: the batches must add up to the sample rate over time, within one sample.
        // Counted from the second frame: the first one after power on is a short one (it
        // ends where the first vertical blanking starts).
        const double expected = (frames - 1) * av.timing.sample_rate / av.timing.fps;
        if (std::fabs(static_cast<double>(audioFrames - audioFramesFirst) - expected) > 1.0)
            fail("audio %llu frames after the first, expected %.2f", static_cast<unsigned long long>(audioFrames - audioFramesFirst), expected);
        if (stateSize == 0) fail("serialize size is zero");

        // Restoring the half-way state and running the second half again must reproduce every frame.
        if (!stateAtHalf.empty()) {
            if (!c.retro_unserialize(stateAtHalf.data(), stateAtHalf.size())) fail("retro_unserialize failed");
            for (unsigned f = frames / 2; f < frames; ++f) {
                currentFrame = f;
                c.retro_run();
                const uint64_t h = fnv1a(lastFrame.pixels.data(), lastFrame.pixels.size() * 4);
                if (h != hashes[f]) { fail("frame %u differs after restoring the state", f); break; }
            }
            // The machine state after the replay must equal the state after the first run, byte for byte.
            std::vector<uint8_t> replayEnd(stateSize);
            if (!c.retro_serialize(replayEnd.data(), stateSize) || replayEnd != stateAtEnd)
                fail("machine state after the replay differs from the first run");
            // A damaged state must be refused, not half-applied.
            std::vector<uint8_t> broken = stateAtHalf;
            broken[0] ^= 0xFF;
            if (c.retro_unserialize(broken.data(), broken.size())) fail("a damaged state was accepted");

            // The same replay in a core that was shut down and started again: whatever a
            // state must bring back (a cartridge's RAM, rewritten chips) is not there
            // unless the state holds it.
            c.retro_unload_game();
            c.retro_deinit();
            c.retro_init();
            const bool again = c.retro_load_game(content.empty() ? nullptr : &gi);
            for (const auto& [port, dev] : devices) c.retro_set_controller_port_device(port, dev);
            if (!again) fail("the content did not load a second time");
            else if (c.retro_serialize_size() != stateSize) fail("state size differs in a restarted core");
            else if (!c.retro_unserialize(stateAtHalf.data(), stateAtHalf.size())) fail("a restarted core refused the state");
            else {
                // What was loaded is all there: saved again at once, it is the same bytes.
                if (!c.retro_serialize(replayEnd.data(), stateSize) || replayEnd != stateAtHalf)
                    fail("a state loaded into a restarted core does not save back as the same bytes");
                for (unsigned f = frames / 2; f < frames; ++f) {
                    currentFrame = f;
                    c.retro_run();
                    const uint64_t h = fnv1a(lastFrame.pixels.data(), lastFrame.pixels.size() * 4);
                    if (h != hashes[f]) { fail("frame %u differs after restoring the state in a restarted core", f); break; }
                }
                if (!c.retro_serialize(replayEnd.data(), stateSize) || replayEnd != stateAtEnd)
                    fail("machine state after the replay in a restarted core differs from the first run");
            }
        }
    }

    if (!dump.empty() && !writePpm(dump.c_str())) fail("could not write %s", dump.c_str());

    if (!expectScreen.empty()) {
        const auto* ram = static_cast<const uint8_t*>(c.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
        if (!ram || c.retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) != 65536) {
            fail("system RAM is not exposed");
        } else {
            std::string screen;
            for (int i = 0; i < 1000; ++i) {
                const uint8_t sc = ram[0x0400 + i] & 0x7F;
                screen += sc < 0x20 ? static_cast<char>(sc + 0x40) : sc < 0x40 ? static_cast<char>(sc) : '?';
                if (i % 40 == 39) screen += '\n';
            }
            for (const auto& e : expectScreen)
                if (screen.find(e) == std::string::npos) fail("text not on the screen: \"%s\"\n%s", e.c_str(), screen.c_str());
        }
    }

    if (expectHz > 0.0) {
        const Tone t = measureTone(av.timing.sample_rate);
        std::printf("audio: %.2f Hz at %.1f dB\n", t.hz, t.db);
        if (std::fabs(t.hz - expectHz) > expectHz * 0.001) fail("tone of %.2f Hz, expected %.2f", t.hz, expectHz);
        if (t.db < expectDbMin || t.db > expectDbMax) fail("level %.1f dB, expected %.1f .. %.1f", t.db, expectDbMin, expectDbMax);
        if (audioChannelsDiffer) fail("left and right channels differ");
    }
    if (expectLevel) {
        const Tone t = measureTone(av.timing.sample_rate);
        std::printf("audio: level %.1f dB\n", t.db);
        if (t.db < levelMin || t.db > levelMax) fail("level %.1f dB, expected %.1f .. %.1f", t.db, levelMin, levelMax);
    }
    if (ledOnCount < expectLed) fail("LED 0 was switched on %u time(s), expected at least %u", ledOnCount, expectLed);
    if (expectSilence) {
        size_t loud = 0;
        for (int16_t v : audioLeft) loud += v != 0;
        if (loud) fail("%zu audio samples are not zero", loud);
    }

    c.retro_unload_game();
    c.retro_deinit();
    dlclose(c.handle);
    if (check) std::printf(failures ? "conformance: %d failure(s)\n" : "conformance: ok\n", failures);
    return failures ? 1 : 0;
}
