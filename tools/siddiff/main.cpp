// siddiff: runs our SID and the reference engine side by side on the same random register
// traffic and compares their digital state after every cycle: per voice the accumulator,
// the noise register, the 12-bit waveform output, OSC, the pulse level, the envelope counter
// and its counters; the value left on the data bus; and what a read of every register
// returns.
//
//   siddiff [--model 6581|8580] [--seeds N] [--cycles N] [--waves LIST] [--every N] [--verbose]
//   siddiff --score [SAMPLE_FOLDER]
//   siddiff --dacs
//   siddiff --filter
//
// --dacs compares the waveform and envelope DAC tables and the voices' zero levels with the
// reference's, code by code; exit status 1 on any difference.
//
// --score prints how the model of the combined waveforms (triangle, sawtooth and pulse
// shorted on the DAC input lines) compares with chip samples: for each chip and
// combination, how many of the 4096 accumulator values give the same OSC byte as the
// reference engine's sampled chip and, with SAMPLE_FOLDER (the suite's SID/resid-test),
// as the suite's two samplings of another chip of each type. The last column is how many
// values those chips disagree on among themselves: what "exact" can mean here. (On the
// 6581 with sawtooth and triangle both selected the accumulator's top bit is pulled low:
// the samples' upper half repeats the lower one, and so does the model's.)
//
// --waves LIST  the waveform selections (control bits 7-4, hex digits) a trace may write;
//               default 0124 89ac: everything whose output follows from rules. The
//               combinations of triangle, sawtooth and pulse (3 5 6 7 b d e f) depend on
//               the model of the shorted waveform lines.
// --every N     a register write about every N cycles (default 16; 2 = dense traffic, large
//               values leave the chip alone long enough for envelopes to run through and
//               for the slow fades: noise register under TEST, floating output, bus value).
// Exit status 1 at the first difference, with the cycle and both states. The last line
// counts what the traces exercised (noise shifts, hard syncs, envelope steps, delayed
// decrements, zero freezes, write-backs into the noise register, TEST releases, bus fades):
// a count of 0 means that rule was not compared.
//
// Some sequences are kept out of the traffic: releasing TEST with a combination with noise
// selected, where what the noise register latches at that moment follows the suite's
// write-back programs here (hw/sid.md W4b) and a simpler rule in the reference engine
// (which fails those programs). latchDiffers() states both rules; the last line counts how
// often a trace was steered around such a release.
//
// --filter measures the analog part of both engines with tones and compares the numbers
// in bands (the two are different models of the same circuit, and real chips differ far
// more than these bands): the low-pass cutoff frequency at several FC values, the height
// of the resonance peak, how the output level follows the volume register, how far the
// output's resting level moves with it (what makes volume-register samples audible), the
// level through the filter against the level around it, and the output voltage of a tone.
// The printed numbers are also kept in tests/filter-score.txt: the gate compares the whole
// output with it, so a change of the model that stays inside the bands still shows.
//
// The reference engine's internals are read directly (they are protected members there).
// (The state a gate change is heading for is compared only while that change is under way:
// afterwards the field has no effect, and the two keep different leftovers over a reset.)
#define protected public
#define private public
#include "resid/sid.h"   // the reference engine's
#undef protected
#undef private

#include "../../src/core/sid.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Rng {
    uint64_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<uint32_t>(s >> 11);
    }
    unsigned below(unsigned n) { return next() % n; }
};

// Whether, when TEST is released while the selection changes from prev to next, the
// register latches something else here than in the reference engine.
bool latchDiffers(bool old6581, unsigned prev, unsigned next) {
    if (prev <= 8) return false;
    // The reference: the old lines, except from noise + pulse (6581: never; 8580: only to
    // $9 and $E) and, on the 6581, between triangle alone and sawtooth alone.
    bool theirs = true;
    if (prev == 0xC) theirs = !old6581 && (next == 0x9 || next == 0xE);
    if (old6581 && (((prev & 3) == 1 && (next & 3) == 2) || ((prev & 3) == 2 && (next & 3) == 1))) theirs = false;
    // Here: 0 = nothing, 1 = the old lines, 2 = something else (always a difference).
    int ours = 0;
    const bool held = (prev & 3) != 0;
    if (old6581) {
        if (held && (prev & next & 3)) ours = 1;
        else if (held && (prev & 4) && (next & 4)) ours = 2;
    } else if (held) {
        if (next != 8 || (prev & 3) == 3) ours = 1;
    } else if (prev == 0xC) {
        if ((next & 3) == 3) ours = 2;
        else if (next == 0x9 || next == 0xE) ours = 1;
    }
    return ours == 2 || (ours == 1) != theirs;
}

struct Row {
    uint32_t acc, noise;
    unsigned out, osc, pulse, env, envRead, rate, rateCompare, rateMatch, expCounter, expPeriod, state, nextState, holdZero;
    unsigned envPipeline, expPipeline, statePipeline;
    bool operator==(const Row&) const = default;
};

Row ours(const k64::Sid::Voice& v) {
    return {v.acc, v.noise, v.out, v.osc, v.pulseOut, v.env, v.envRead, v.rate, v.rateCompare, v.rateMatch, v.expCounter, v.expPeriod,
            v.state, v.statePipeline ? v.nextState : 0u, v.holdZero, v.envPipeline, v.expPipeline, v.statePipeline};
}

Row theirs(reSID::Voice& v) {
    auto& w = v.wave;
    auto& e = v.envelope;
    // (While TEST lets the noise register's cells drift to one, the reference's value runs
    // over into bit 23; the register has 23 bits.)
    return {w.accumulator, w.shift_register & 0x7FFFFF, w.waveform_output, w.osc3, w.pulse_output, e.envelope_counter, e.env3,
            e.rate_counter, e.rate_period, e.reset_rate_counter ? 1u : 0u, e.exponential_counter, e.exponential_counter_period,
            static_cast<unsigned>(e.state), e.state_pipeline ? static_cast<unsigned>(e.next_state) : 0u, e.hold_zero ? 1u : 0u,
            static_cast<unsigned>(e.envelope_pipeline), static_cast<unsigned>(e.exponential_pipeline),
            static_cast<unsigned>(e.state_pipeline)};
}

void print(const char* who, int voice, const Row& r) {
    std::printf("  %s voice %d: acc %06x noise %06x out %03x osc %03x pulse %03x | env %02x (read %02x) rate %04x/%04x match %u exp %u/%u "
                "state %u->%u hold %u pipes env %u exp %u state %u\n",
                who, voice + 1, r.acc, r.noise, r.out, r.osc, r.pulse, r.env, r.envRead, r.rate, r.rateCompare, r.rateMatch, r.expCounter,
                r.expPeriod, r.state, r.nextState, r.holdZero, r.envPipeline, r.expPipeline, r.statePipeline);
}


std::vector<int> loadSample(const std::string& folder, int set, int chip, unsigned wf) {
    const std::string name = folder + "/oscsample" + std::to_string(set) + (chip ? "-8580wf" : "-6581wf") + std::to_string(wf) + "0.dat";
    std::vector<int> out;
    FILE* f = std::fopen(name.c_str(), "rb");
    if (!f) return out;
    unsigned char buf[4098];
    if (std::fread(buf, 1, sizeof buf, f) == sizeof buf) out.assign(buf + 2, buf + 4098);   // after the load address
    std::fclose(f);
    return out;
}

int score(const std::string& folder) {
    static const char* const kName[8] = {"", "", "", "sawtooth+triangle", "", "pulse+triangle", "pulse+sawtooth", "pulse+sawtooth+triangle"};
    std::printf("combined waveforms: accumulator values (of 4096) whose OSC byte equals the samples\n");
    std::printf("%-5s %-24s %10s %9s %9s %12s\n", "chip", "combination", "reference", "suite 0", "suite 1", "chips differ");
    reSID::WaveformGenerator init;   // fills the reference engine's tables
    for (int chip = 0; chip < 2; ++chip) {
        const k64::SidModel model = chip ? k64::SidModel::Mos8580 : k64::SidModel::Mos6581;
        for (unsigned wf : {3u, 5u, 6u, 7u}) {
            const unsigned n = 4096;
            const std::vector<int> s0 = folder.empty() ? std::vector<int>() : loadSample(folder, 0, chip, wf);
            const std::vector<int> s1 = folder.empty() ? std::vector<int>() : loadSample(folder, 1, chip, wf);
            unsigned ref = 0, m0 = 0, m1 = 0, differ = 0;
            for (unsigned i = 0; i < n; ++i) {
                const int ours = k64::Sid::combinedWave(model, wf, i) >> 4;
                const int theirs = reSID::WaveformGenerator::model_wave[chip][wf][i] >> 4;
                ref += ours == theirs;
                if (!s0.empty()) { m0 += ours == s0[i]; differ += theirs != s0[i]; }
                if (!s1.empty()) m1 += ours == s1[i];
            }
            std::printf("%-5s %-24s %10u", chip ? "8580" : "6581", kName[wf], ref);
            if (s0.empty() || s1.empty()) std::printf("\n");
            else std::printf(" %9u %9u %12u\n", m0, m1, differ);
        }
        std::printf("%-5s smallest distance of a line voltage from the threshold: %.3g of the supply voltage\n", chip ? "8580" : "6581",
                    k64::Sid::combinedWaveMargin(model));
    }
    return 0;
}


// ---- the analog part, measured with tones -------------------------------------------------
constexpr double kPi = 3.14159265358979323846;
constexpr double kFilterClock = 985248.0;

struct OurAnalog {
    k64::Sid sid;
    double volts;
    explicit OurAnalog(int chip) : sid(chip ? k64::SidModel::Mos8580 : k64::SidModel::Mos6581), volts(k64::Sid::audioFullScaleVolts(sid.model())) {
        sid.setClockHz(kFilterClock);
    }
    void reg(unsigned r, unsigned v) { sid.write(r, static_cast<uint8_t>(v)); }
    double step(int voice1) { return sid.clockFilter(voice1, 0, 0) * volts; }   // in volts
};
struct TheirAnalog {
    reSID::SID sid;
    explicit TheirAnalog(int chip) {
        sid.set_chip_model(chip ? reSID::MOS8580 : reSID::MOS6581);
        sid.set_sampling_parameters(kFilterClock, reSID::SAMPLE_INTERPOLATE, 44100);
        sid.reset();
        sid.adjust_filter_bias(0.0);   // values the engine sets only in its first instance
    }
    void reg(unsigned r, unsigned v) { sid.write(r, v); }
    double step(int voice1) {   // in volts: its output is scaled by vo_N16 per volt
        sid.filter.clock(voice1, 0, 0);
        return sid.filter.output() / reSID::Filter::model_filter[sid.sid_model].vo_N16;
    }
};

// Amplitude of the response to a tone on voice 1 (amp in voice units), after settling,
// over a whole number of periods; mean = the output's resting level meanwhile.
template <class Device>
double respond(Device& d, double hz, double amp, double* mean = nullptr) {
    const double periods = std::ceil(std::fmax(3.0, 0.02 * hz));
    const int n = static_cast<int>(periods * kFilterClock / hz + 0.5);
    const int settle = static_cast<int>(kFilterClock * 0.05) + 2 * static_cast<int>(kFilterClock / hz);
    double s = 0, c = 0, sum = 0;
    for (int i = 0; i < settle + n; ++i) {
        const double w = 2 * kPi * hz * i / kFilterClock;
        const double y = d.step(static_cast<int>(amp * std::sin(w)));
        if (i >= settle) { s += y * std::sin(w); c += y * std::cos(w); sum += y; }
    }
    if (mean) *mean = sum / n;
    return 2 * std::sqrt(s * s + c * c) / n;
}

constexpr double kTone = 2048.0 * 255.0 * 0.25;   // a quarter of a voice's range

template <class Device>
double cutoffHz(int chip, unsigned fc) {
    Device d(chip);
    d.reg(0x15, fc & 7); d.reg(0x16, fc >> 3); d.reg(0x17, 0x01); d.reg(0x18, 0x1F);
    const double low = respond(d, 20.0, kTone) * 0.70710678;
    double a = 20.0, b = 40000.0;   // the response falls with frequency: bisect for -3 dB
    for (int i = 0; i < 16; ++i) {
        const double m = std::sqrt(a * b);
        if (respond(d, m, kTone) > low) a = m; else b = m;
    }
    return std::sqrt(a * b);
}

template <class Device>
double resonancePeak(int chip, unsigned res) {
    const unsigned fc = 1024;
    Device d(chip);
    d.reg(0x15, fc & 7); d.reg(0x16, fc >> 3); d.reg(0x17, (res << 4) | 0x01); d.reg(0x18, 0x2F);
    double peak = 0;
    for (double hz = 500; hz < 16000; hz *= 1.04) peak = std::fmax(peak, respond(d, hz, kTone * 0.1));
    Device e(chip);
    e.reg(0x15, fc & 7); e.reg(0x16, fc >> 3); e.reg(0x17, (res << 4) | 0x01); e.reg(0x18, 0x1F);
    return peak / respond(e, 30.0, kTone * 0.1);
}

// Level of an unfiltered voice at a volume, relative to volume 15; the travel of the resting
// level from volume 0 to 15 over that tone's amplitude; the level through the low-pass.
template <class Device>
void levels(int chip, double out[5]) {
    double gain[16] = {}, rest[16] = {};
    for (unsigned vol : {0u, 1u, 8u, 15u}) {
        Device d(chip);
        d.reg(0x17, 0x00); d.reg(0x18, vol);
        gain[vol] = respond(d, 1000.0, kTone, &rest[vol]);
    }
    Device f(chip);
    f.reg(0x15, 7); f.reg(0x16, 255); f.reg(0x17, 0x01); f.reg(0x18, 0x1F);
    out[0] = gain[1] / gain[15];
    out[1] = gain[8] / gain[15];
    out[2] = (rest[15] - rest[0]) / gain[15];
    out[3] = respond(f, 200.0, kTone) / gain[15];
    out[4] = gain[15];
}

int filterBands() {
    int bad = 0;
    auto band = [&bad](const char* chip, const char* what, double ours, double theirs, double percent) {
        const double off = (ours / theirs - 1.0) * 100.0;
        const bool ok = std::fabs(off) <= percent;
        std::printf("%s %-46s ours %10.4f  reference %10.4f  %+6.1f %% (allowed %.0f)%s\n", chip, what, ours, theirs, off, percent, ok ? "" : "  FAIL");
        bad += !ok;
    };
    for (int chip = 0; chip < 2; ++chip) {
        const char* name = chip ? "8580" : "6581";
        char what[80];
        // The 6581's knee (FC 512-896) is left out: there the reference's small currents
        // are rounded to zero by its 16-bit table, ours are not.
        const unsigned fcs[2][4] = {{0, 1024, 1536, 2047}, {32, 256, 1024, 2047}};
        for (unsigned fc : fcs[chip]) {
            std::snprintf(what, sizeof what, "low-pass cutoff at FC %u (Hz)", fc);
            band(name, what, cutoffHz<OurAnalog>(chip, fc), cutoffHz<TheirAnalog>(chip, fc), chip ? 3 : 12);
        }
        for (unsigned res : {0u, 8u, 15u}) {
            std::snprintf(what, sizeof what, "resonance peak over low-pass level, RES %u", res);
            band(name, what, resonancePeak<OurAnalog>(chip, res), resonancePeak<TheirAnalog>(chip, res), chip ? 10 : 25);
        }
        double a[5], b[5];
        levels<OurAnalog>(chip, a);
        levels<TheirAnalog>(chip, b);
        band(name, "a quarter-range tone at volume 15 (V at the pin)", a[4], b[4], chip ? 2 : 10);
        band(name, "level at volume 1 over volume 15", a[0], b[0], chip ? 2 : 6);
        band(name, "level at volume 8 over volume 15", a[1], b[1], chip ? 2 : 6);
        band(name, "resting level travel, volume 0 to 15, over a tone", a[2], b[2], chip ? 5 : 15);
        band(name, "level through the low-pass over around it", a[3], b[3], chip ? 3 : 6);
    }
    return bad ? 1 : 0;
}

// The DAC tables and the voice zero levels against the reference's, code by code.
int dacs() {
    int bad = 0;
    for (int chip = 0; chip < 2; ++chip) {
        const k64::SidModel model = chip ? k64::SidModel::Mos8580 : k64::SidModel::Mos6581;
        reSID::SID sid;
        sid.set_chip_model(chip ? reSID::MOS8580 : reSID::MOS6581);
        int wave = 0, env = 0;
        for (unsigned c = 0; c < 4096; ++c) wave += k64::Sid::waveDac(model, c) != reSID::WaveformGenerator::model_dac[chip][c];
        for (unsigned c = 0; c < 256; ++c) env += k64::Sid::envelopeDac(model, c) != reSID::EnvelopeGenerator::model_dac[chip][c];
        // The zero level, through a voice's output: waveform code 0, envelope at $FF.
        k64::Sid ours(model);
        k64::Sid::State st = ours.state();
        st.voice[0].out = 0; st.voice[0].env = 0xFF;
        ours.setState(st);
        sid.voice[0].wave.waveform_output = 0;
        sid.voice[0].envelope.envelope_counter = 0xFF;
        const int zero = ours.voiceOutput(0) != sid.voice[0].output();
        std::printf("%s DACs: %d of 4096 waveform codes differ, %d of 256 envelope codes, zero level %s\n", chip ? "8580" : "6581", wave, env,
                    zero ? "DIFFERS" : "equal");
        bad += wave + env + zero;
    }
    return bad ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--score") return score(argc >= 3 ? argv[2] : "");
    if (argc >= 2 && std::string(argv[1]) == "--dacs") return dacs();
    if (argc >= 2 && std::string(argv[1]) == "--filter") return filterBands();
    k64::SidModel model = k64::SidModel::Mos6581;
    int seeds = 8;
    long cycles = 200000;
    std::string waves = "012489ac";
    bool verbose = false;
    unsigned every = 16;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--model" && i + 1 < argc) model = std::string(argv[++i]) == "8580" ? k64::SidModel::Mos8580 : k64::SidModel::Mos6581;
        else if (a == "--seeds" && i + 1 < argc) seeds = std::atoi(argv[++i]);
        else if (a == "--cycles" && i + 1 < argc) cycles = std::atol(argv[++i]);
        else if (a == "--waves" && i + 1 < argc) waves = argv[++i];
        else if (a == "--every" && i + 1 < argc) every = static_cast<unsigned>(std::atol(argv[++i]));
        else if (a == "--verbose") verbose = true;
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    std::vector<unsigned> allowed;
    for (char c : waves) allowed.push_back(static_cast<unsigned>(std::strtoul(std::string(1, c).c_str(), nullptr, 16)));
    if (allowed.empty()) { std::fprintf(stderr, "siddiff: no waveforms\n"); return 2; }

    long compared = 0;
    long nShift = 0, nSync = 0, nEnvStep = 0, nPipe = 0, nFreeze = 0, nWriteBack = 0, nTestOff = 0, nFade = 0, nDelayBug = 0;
    long nNoiseReset = 0, nFloat = 0, nSustain = 0, nSteered = 0, nGateInStep = 0, nChipReset = 0;
    for (int seed = 1; seed <= seeds; ++seed) {
        Rng rng{0x9E3779B97F4A7C15ull * static_cast<uint64_t>(seed) + 1};
        k64::Sid a(model);
        static reSID::SID b;
        b.set_chip_model(model == k64::SidModel::Mos8580 ? reSID::MOS8580 : reSID::MOS6581);
        // Cycle-exact mode of the reference: one clock() per cycle, writes at once.
        b.set_sampling_parameters(985248, reSID::SAMPLE_INTERPOLATE, 44100);
        // Both start from the same power-on state.
        for (int v = 0; v < 3; ++v) {
            b.voice[v].wave.accumulator = 0x555555;
            b.voice[v].wave.tri_saw_pipeline = 0x555;
            b.voice[v].envelope.envelope_counter = 0xAA;
            b.voice[v].envelope.env3 = 0xAA;
            b.voice[v].envelope.next_state = reSID::EnvelopeGenerator::RELEASE;
        }
        b.reset();
        a = k64::Sid(model);
        // Per seed: how often frequencies are high (noise and sync activity) and envelopes fast.
        const bool fast = rng.below(2) != 0;
        for (long c = 0; c < cycles; ++c) {
            if (rng.below(every) == 0) {
                unsigned reg = rng.below(0x15);
                unsigned value = rng.below(256);
                switch (reg % 7) {
                case 1: if (!fast && rng.below(4)) value &= 0x0F; break;          // mostly low frequencies
                case 4: value = (allowed[rng.below(static_cast<unsigned>(allowed.size()))] << 4) | (value & 0x07) | (rng.below(8) == 0 ? 0x08 : 0);
                        if (rng.below(4) == 0) value &= 0x0F;   // often no waveform: the output floats
                        break;
                case 5: case 6: if (fast || rng.below(2)) value &= 0x33; break;   // short envelopes are stepped more
                default: break;
                }
                if (reg % 7 == 4) {
                    const auto& v = a.state().voice[reg / 7];
                    if (v.test && !(value & 0x08)) {
                        // (Steered by leaving TEST set: no release, nothing latched.)
                        if (latchDiffers(model == k64::SidModel::Mos6581, v.waveform, value >> 4)) { value |= 0x08; ++nSteered; }
                        else ++nTestOff;
                    }
                    if ((value & 1) != v.gate && (v.rateMatch || v.expPipeline || v.envPipeline)) ++nGateInStep;
                }
                a.write(reg, static_cast<uint8_t>(value));
                b.write(reg, value);
            }
            if (rng.below(every * 3000) == 0) {   // now and then the RES pin
                if (a.state().voice[0].env || a.state().voice[1].env || a.state().voice[2].env) ++nChipReset;
                a.reset();
                b.reset();
            }
            if (rng.below(every * 4) == 0) {
                const unsigned reg = rng.below(0x20);
                const unsigned x = a.read(reg), y = b.read(reg);
                if (x != y) {
                    std::printf("DIFFERS seed %d cycle %ld: read of register $%02x: ours %02x, reference %02x\n", seed, c, reg, x, y);
                    return 1;
                }
            }
            k64::Sid::State before = a.state();
            a.clock();
            b.clock();
            for (int v = 0; v < 3; ++v) {
                const auto& p = before.voice[v];
                const auto& q = a.state().voice[v];
                if (q.noise != p.noise) { if (q.noise == (((p.noise << 1) | (((p.noise >> 22) ^ (p.noise >> 17)) & 1)) & 0x7FFFFF)) ++nShift; else ++nWriteBack; }
                if (!p.test && p.freq && q.acc == 0 && p.acc != 0 && ((p.acc + p.freq) & 0xFFFFFF) != 0) ++nSync;
                if (q.env != p.env) ++nEnvStep;
                if (q.envPipeline && !p.envPipeline) ++nPipe;
                if (q.holdZero && !p.holdZero) ++nFreeze;
                if (p.rate == 0x7FFF) ++nDelayBug;
                if (p.noiseReset == 1 && p.test) ++nNoiseReset;
                if (p.floatingTtl == 1 && !p.waveform && !q.out) ++nFloat;
                if (q.state == k64::Sid::DecaySustain && q.env == q.sustain * 0x11 && p.expPipeline == 1 && !q.envPipeline && q.env != 0 && q.env != 0xFF) ++nSustain;
            }
            if (before.bus && !a.state().bus && before.busTtl == 1) ++nFade;
            if (a.state().bus != b.bus_value) {
                std::printf("DIFFERS seed %d cycle %ld: bus value: ours %02x, reference %02x\n", seed, c, a.state().bus, b.bus_value);
                return 1;
            }
            for (int v = 0; v < 3; ++v) {
                const Row x = ours(a.state().voice[v]), y = theirs(b.voice[v]);
                if (!(x == y)) {
                    std::printf("DIFFERS seed %d cycle %ld (control %02x %02x %02x):\n", seed, c, a.state().reg[4], a.state().reg[11], a.state().reg[18]);
                    for (int k = 0; k < 3; ++k) {
                        print("ours     ", k, ours(a.state().voice[k]));
                        print("reference", k, theirs(b.voice[k]));
                    }
                    return 1;
                }
            }
            ++compared;
        }
        if (verbose) std::printf("seed %d: %ld cycles identical\n", seed, cycles);
    }
    std::printf("siddiff: %s, %d seeds, %ld cycles compared, identical\n", model == k64::SidModel::Mos8580 ? "8580" : "6581", seeds, compared);
    std::printf("siddiff: exercised: %ld noise shifts, %ld hard syncs, %ld envelope steps, %ld delayed decrements, %ld zero freezes, "
                "%ld noise write-backs, %ld TEST releases, %ld rate counter wraps, %ld bus fades, %ld noise resets by TEST, "
                "%ld floating outputs faded, %ld steps held at the sustain level, %ld gate changes during an envelope step; "
                "%ld chip resets with an envelope not at zero; %ld TEST releases steered around\n",
                nShift, nSync, nEnvStep, nPipe, nFreeze, nWriteBack, nTestOff, nDelayBug, nFade, nNoiseReset, nFloat, nSustain, nGateInStep,
                nChipReset, nSteered);
    return 0;
}
