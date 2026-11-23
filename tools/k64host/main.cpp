// k64host: a minimal libretro frontend that loads a built core and checks it
// through the API only, the way a real frontend does.
//
//   k64host CORE.so [--content FILE] [--frames N] [--option KEY=VALUE]...
//                   [--system DIR] [--save DIR] [--dump FILE.ppm] [--check]
//
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
    SYM(retro_unserialize)
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
uint64_t audioFrames = 0;
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

void audioSample(int16_t, int16_t) { ++audioFrames; }
size_t audioBatch(const int16_t*, size_t frames) {
    audioFrames += frames;
    return frames;
}
void inputPoll() {}
int16_t inputState(unsigned, unsigned, unsigned, unsigned) { return 0; }

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
    LOAD(retro_unserialize)
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
        c.retro_run();
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
        // Audio: the batch per frame must add up to the sample rate over time, within one sample.
        const double expected = frames * av.timing.sample_rate / av.timing.fps;
        if (std::fabs(static_cast<double>(audioFrames) - expected) > 1.0)
            fail("audio %llu frames, expected %.2f", static_cast<unsigned long long>(audioFrames), expected);
        if (stateSize == 0) fail("serialize size is zero");

        // Restoring the half-way state and running the second half again must reproduce every frame.
        if (!stateAtHalf.empty()) {
            if (!c.retro_unserialize(stateAtHalf.data(), stateAtHalf.size())) fail("retro_unserialize failed");
            for (unsigned f = frames / 2; f < frames; ++f) {
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

    c.retro_unload_game();
    c.retro_deinit();
    dlclose(c.handle);
    if (check) std::printf(failures ? "conformance: %d failure(s)\n" : "conformance: ok\n", failures);
    return failures ? 1 : 0;
}
