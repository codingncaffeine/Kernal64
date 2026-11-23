// siddiff: runs our SID and the reference engine side by side on the same random register
// traffic and compares their digital state after every cycle: per voice the accumulator,
// the noise register, the 12-bit waveform output, OSC, the pulse level, the envelope counter
// and its counters; the value left on the data bus; and what a read of every register
// returns.
//
//   siddiff [--model 6581|8580] [--seeds N] [--cycles N] [--waves LIST] [--every N] [--verbose]
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
// The reference engine's internals are read directly (they are protected members there).
#define protected public
#define private public
#include "src/sid.h"   // the reference engine's (its folder is on the include path)
#undef protected
#undef private

#include "../../src/core/sid.h"

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

struct Row {
    uint32_t acc, noise;
    unsigned out, osc, pulse, env, rate, ratePeriod, expCounter, expPeriod, state, holdZero, envPipeline;
    bool operator==(const Row&) const = default;
};

Row ours(const k64::Sid::Voice& v) {
    return {v.acc, v.noise, v.out, v.osc, v.pulseOut, v.env, v.rate, v.ratePeriod, v.expCounter, v.expPeriod, v.state, v.holdZero, v.envPipeline};
}

Row theirs(reSID::Voice& v) {
    auto& w = v.wave;
    auto& e = v.envelope;
    return {w.accumulator, w.shift_register, w.waveform_output, w.osc3, w.pulse_output, e.envelope_counter, e.rate_counter,
            e.rate_period, e.exponential_counter, e.exponential_counter_period, static_cast<unsigned>(e.state),
            e.hold_zero ? 1u : 0u, static_cast<unsigned>(e.envelope_pipeline)};
}

void print(const char* who, int voice, const Row& r) {
    std::printf("  %s voice %d: acc %06x noise %06x out %03x osc %03x pulse %03x | env %02x rate %04x/%04x exp %u/%u state %u hold %u pipe %u\n",
                who, voice + 1, r.acc, r.noise, r.out, r.osc, r.pulse, r.env, r.rate, r.ratePeriod, r.expCounter, r.expPeriod,
                r.state, r.holdZero, r.envPipeline);
}

} // namespace

int main(int argc, char** argv) {
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
    long nNoiseReset = 0, nFloat = 0, nSustain = 0;
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
                if (reg % 7 == 4 && a.state().voice[reg / 7].test && !(value & 0x08)) ++nTestOff;
                a.write(reg, static_cast<uint8_t>(value));
                b.write(reg, value);
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
                if (p.floatingTtl == 1 && !p.waveform) ++nFloat;
                if (q.state == k64::Sid::DecaySustain && q.env == q.sustain * 0x11 && q.rate == 0 && p.rate != 0 && q.env != 0 && q.env != 0xFF) ++nSustain;
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
                "%ld floating outputs faded, %ld steps held at the sustain level\n",
                nShift, nSync, nEnvStep, nPipe, nFreeze, nWriteBack, nTestOff, nDelayBug, nFade, nNoiseReset, nFloat, nSustain);
    return 0;
}
