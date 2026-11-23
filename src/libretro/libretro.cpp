// libretro entry points.
#include "../core/c64.h"
#include "../core/roms.h"
#include "libretro.h"

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

void log(retro_log_level level, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (logCb) logCb(level, "[Kernal64] %s\n", buf);
    else std::fprintf(stderr, "[Kernal64] %s\n", buf);
}

k64::VicModel modelFromOptions() {
    retro_variable var{"kernal64_model", nullptr};
    if (envCb && envCb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!std::strcmp(var.value, "ntsc")) return k64::VicModel::Mos6567R8;
        if (!std::strcmp(var.value, "ntsc-old")) return k64::VicModel::Mos6567R56A;
    }
    return k64::VicModel::Mos6569;
}

void fillAvInfo(retro_system_av_info* info) {
    const auto& t = machine ? machine->timing() : k64::kTimingPal;
    info->geometry.base_width = static_cast<unsigned>(t.visibleWidth);
    info->geometry.base_height = static_cast<unsigned>(t.visibleLines);
    info->geometry.max_width = k64::kMaxRasterWidth;
    info->geometry.max_height = k64::kMaxRasterHeight;
    info->geometry.aspect_ratio = static_cast<float>(t.visibleWidth * t.pixelAspect() / t.visibleLines);
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
        {"kernal64_model", "Machine; pal|ntsc|ntsc-old"},
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
    audioCarry = 0.0;
    log(RETRO_LOG_INFO, "version %s, machine %s", KERNAL64_VERSION, machine->timing().name);

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
    return machine && machine->timing().model != k64::VicModel::Mos6569 ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

RETRO_API void retro_run(void) {
    bool updated = false;
    if (envCb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        const k64::VicModel m = modelFromOptions();
        if (m != machine->timing().model) {
            machine->setModel(m);
            retro_system_av_info av{};
            fillAvInfo(&av);
            envCb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
        }
    }
    if (inputPollCb) inputPollCb();

    if (machine->hasSystemRoms()) machine->runFrame();

    const auto& t = machine->timing();
    machine->visibleFrame(frameOut.data());
    videoCb(frameOut.data(), static_cast<unsigned>(t.visibleWidth), static_cast<unsigned>(t.visibleLines),
            static_cast<size_t>(t.visibleWidth) * sizeof(uint32_t));

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
RETRO_API void* retro_get_memory_data(unsigned) { return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned) { return 0; }

} // extern "C"
