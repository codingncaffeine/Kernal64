#include "options.h"

#include "keys.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace k64lr {

int keyIndex(const char* id) {
    for (int i = 0; i < kC64KeyCount; ++i)
        if (!std::strcmp(kC64Keys[i].id, id)) return i;
    return -1;
}

namespace {

struct Action { const char* value; const char* label; PadAction action; };
constexpr Action kActions[] = {
    {"none", "Nothing", kNone},
    {"fire", "Joystick fire", kFire},
    {"turbo", "Joystick fire, repeating", kTurbo},
    {"up", "Joystick up", kUp},
    {"keyboard", "On-screen keyboard", kKeyboard},
    {"tape_play", "Datasette PLAY / STOP", kTapePlay},
    {"tape_record", "Datasette RECORD", kTapeRecord},
    {"tape_rewind", "Datasette rewind", kTapeRewind},
    {"freeze", "Freezer cartridge button", kFreeze},
    {"swap", "Swap the joystick ports", kSwapPorts},
};

struct Def {
    const char* key;
    const char* desc;
    const char* info;
    const char* category;
    std::vector<std::pair<std::string, std::string>> values;   // value, label ("" = the value)
    const char* defaultValue;
};

// Everything the strings point at lives here for the core's lifetime.
std::vector<Def>& defs() {
    static std::vector<Def> d;
    if (!d.empty()) return d;
    d.push_back({"kernal64_model", "Video chip", "The VIC-II, and with it the video standard and the machine's clock. "
                 "Automatic: PAL unless the content's file name says NTSC or USA. A change restarts the machine.",
                 "machine",
                 {{"auto", "Automatic"}, {"pal", "PAL 6569"}, {"pal-new", "PAL 8565 (C64C)"}, {"pal-old", "PAL 6569R1 (first revision)"},
                  {"ntsc", "NTSC 6567R8"}, {"ntsc-new", "NTSC 8562 (C64C)"}, {"ntsc-old", "NTSC 6567R56A (first revision)"}},
                 "auto"});
    d.push_back({"kernal64_cia", "CIA chips", "The two 6526s of the first machines, or the 8521s of the C64C "
                 "(their timer interrupts come a cycle earlier).", "machine",
                 {{"6526", "6526 (C64)"}, {"8521", "8521 (C64C)"}}, "6526"});
    d.push_back({"kernal64_sid", "SID chip", "The 6581 of the first machines or the 8580 of the C64C: they differ in "
                 "filter, combined waveforms and the volume register's sample playback.", "machine",
                 {{"6581", "6581 (C64)"}, {"8580", "8580 (C64C)"}}, "6581"});
    d.push_back({"kernal64_sid_noise_pulse", "6581 noise + pulse", "A way in which 6581 chips differ: noise selected "
                 "together with pulse drains the noise register on most chips.", "machine",
                 {{"drains it", "Drains the noise register"}, {"leaves it alone", "Leaves it alone"}}, "drains it"});
    d.push_back({"kernal64_border", "Border", "How much of the border is shown: all the chip draws, a frame around the "
                 "screen, or the 320x200 screen alone.", "video",
                 {{"full", "Full"}, {"small", "Small"}, {"none", "None"}}, "full"});
    d.push_back({"kernal64_fast_loading", "Fast loading", "While a tape moves (and, with disks, while the 1541's motor "
                 "runs), the machine runs 4 times as fast, sound muted; normal speed as soon as the loading stops. "
                 "What the machine does is the same, only sooner.", "media",
                 {{"tape_disk", "Tape and disk"}, {"tape", "Tape"}, {"off", "Off"}}, "tape_disk"});
    d.push_back({"kernal64_drive8", "Drive 8 (1541)", "The 1541 on the serial bus. Needs its ROMs in the system folder.",
                 "media", {{"on", "On"}, {"off", "Off"}}, "on"});
    d.push_back({"kernal64_keyboard", "Keyboard", "Symbolic: a key types the symbol printed on it (US layout: SHIFT+8 "
                 "is *, SHIFT+' is \"). Positional: the keys are where the C64's own keys are (SHIFT+8 is the C64's (, "
                 "and so on), as on the real keyboard.", "input",
                 {{"symbolic", "Symbolic"}, {"positional", "Positional"}}, "symbolic"});
    d.push_back({"kernal64_joyport", "Player 1's control port", "The C64 control port player 1's controller is in; "
                 "player 2's is in the other. Most games read port 2.", "input",
                 {{"2", "Port 2"}, {"1", "Port 1"}}, "2"});
    static std::string padKeys[kPadButtonCount];   // the option keys' storage
    for (int n = 0; n < kPadButtonCount; ++n) {
        const PadButton& b = kPadButtons[n];
        Def def{nullptr, b.label, "What the button does: a joystick line, a C64 key or an action.", "input", {}, b.defaultValue};
        padKeys[n] = std::string("kernal64_pad_") + b.name;
        def.key = padKeys[n].c_str();
        for (const Action& a : kActions) def.values.emplace_back(a.value, a.label);
        for (const C64Key& k : kC64Keys) def.values.emplace_back(std::string("key_") + k.id, std::string("Key ") + k.label);
        d.push_back(std::move(def));
    }
    return d;
}

retro_core_option_v2_category kCategories[] = {
    {"machine", "Machine", "The chips the C64 is built with."},
    {"video", "Video", "The picture."},
    {"media", "Media", "Drives and media."},
    {"input", "Input", "Control ports and what the RetroPad's buttons do."},
    {nullptr, nullptr, nullptr},
};

template <typename T>
void fillValues(T& out, const Def& d) {
    size_t i = 0;
    for (const auto& [value, label] : d.values) {
        if (i + 1 >= RETRO_NUM_CORE_OPTION_VALUES_MAX) break;
        out.values[i].value = value.c_str();
        out.values[i].label = label.empty() ? nullptr : label.c_str();
        ++i;
    }
    out.values[i] = {nullptr, nullptr};
}

} // namespace

PadAction padActionFromValue(const char* value) {
    if (!value) return kNone;
    for (const Action& a : kActions)
        if (!std::strcmp(a.value, value)) return a.action;
    if (!std::strncmp(value, "key_", 4)) {
        const int k = keyIndex(value + 4);
        if (k >= 0) return static_cast<PadAction>(kKey + k);
    }
    return kNone;
}

void registerOptions(retro_environment_t env) {
    const auto& d = defs();
    unsigned version = 0;
    if (!env(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version)) version = 0;
    if (version >= 2) {
        static std::vector<retro_core_option_v2_definition> v2(d.size() + 1);
        for (size_t i = 0; i < d.size(); ++i) {
            v2[i] = {};
            v2[i].key = d[i].key;
            v2[i].desc = d[i].desc;
            v2[i].info = d[i].info;
            v2[i].category_key = d[i].category;
            fillValues(v2[i], d[i]);
            v2[i].default_value = d[i].defaultValue;
        }
        v2[d.size()] = {};
        static retro_core_options_v2 opts{kCategories, v2.data()};
        if (env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts)) return;
    }
    if (version >= 1) {
        static std::vector<retro_core_option_definition> v1(d.size() + 1);
        for (size_t i = 0; i < d.size(); ++i) {
            v1[i] = {};
            v1[i].key = d[i].key;
            v1[i].desc = d[i].desc;
            v1[i].info = d[i].info;
            fillValues(v1[i], d[i]);
            v1[i].default_value = d[i].defaultValue;
        }
        v1[d.size()] = {};
        if (env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, v1.data())) return;
    }
    // Plain variables: "Description; default|other|...".
    static std::vector<std::string> texts;
    static std::vector<retro_variable> vars;
    texts.clear();
    vars.clear();
    texts.reserve(d.size());
    for (const Def& def : d) {
        std::string t = std::string(def.desc) + "; " + def.defaultValue;
        for (const auto& v : def.values)
            if (v.first != def.defaultValue) t += "|" + v.first;
        texts.push_back(std::move(t));
    }
    for (size_t i = 0; i < d.size(); ++i) vars.push_back({d[i].key, texts[i].c_str()});
    vars.push_back({nullptr, nullptr});
    env(RETRO_ENVIRONMENT_SET_VARIABLES, vars.data());
}

} // namespace k64lr
