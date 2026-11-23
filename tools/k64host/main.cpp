// k64host: a minimal libretro frontend that loads a built core and checks it
// through the API only, the way a real frontend does.
//
//   k64host CORE.so [--content FILE] [--frames N] [--option KEY=VALUE]...
//                   [--system DIR] [--save DIR] [--dump FILE.ppm] [--check]
//                   [--type TEXT] [--type-at FRAME] [--expect-screen TEXT]
//                   [--expect-tone HZ DB_MIN DB_MAX] [--expect-silence]
//
// --type presses the keys for TEXT one at a time from frame --type-at (default 200),
// each held 3 frames and released 3 ("\n" = RETURN). --expect-screen fails the run
// unless TEXT is in the C64 text screen ($0400), read through RETRO_MEMORY_SYSTEM_RAM.
// --expect-tone fails the run unless the last half second of audio is a tone of HZ (within
// 0.1 %) at a level between DB_MIN and DB_MAX (RMS, dB below full scale); --expect-silence
// unless every sample of the run is zero. The audio is only ever looked at as numbers.
// --check runs the conformance checks and exits non-zero on the first failure.
// Without it the run prints a summary only.
#include "libretro.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    SYM(retro_unserialize) SYM(retro_get_memory_data) SYM(retro_get_memory_size)
#undef SYM
};

std::map<std::string, std::string> options;
std::string systemDir = ".", saveDir = ".";
bool optionsChanged = false;
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

bool environment(unsigned cmd, void* data) {
    switch (cmd) {
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
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* v = static_cast<retro_variable*>(data);
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
std::string typeText;
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
    default: return RETROK_UNKNOWN;
    }
}
void inputPoll() {}
int16_t inputState(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_KEYBOARD || currentFrame < typeAt) return 0;
    const unsigned step = (currentFrame - typeAt) / 6, phase = (currentFrame - typeAt) % 6;
    if (step >= typeText.size() || phase >= 3) return 0;
    return keyFor(typeText[step]) == id ? 1 : 0;
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
    bool expectSilence = false;
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
            typeText = next();
            for (size_t p = 0; (p = typeText.find("\\n", p)) != std::string::npos;) typeText.replace(p, 2, "\n");
        } else if (a == "--type-at") typeAt = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--expect-screen") expectScreen.push_back(next());
        else if (a == "--expect-tone") { expectHz = std::stod(next()); expectDbMin = std::stod(next()); expectDbMax = std::stod(next()); }
        else if (a == "--expect-silence") expectSilence = true;
        else if (a == "--option") {
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
    LOAD(retro_unserialize) LOAD(retro_get_memory_data) LOAD(retro_get_memory_size)
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

    retro_game_info gi{};
    gi.path = content.empty() ? nullptr : content.c_str();
    if (!c.retro_load_game(content.empty() ? nullptr : &gi)) { fail("retro_load_game returned false"); return 1; }
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
