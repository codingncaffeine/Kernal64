#include "sid.h"

#include <array>
#include <cmath>

namespace k64 {

namespace {
// E1: what the rate counter is compared with for the rate values 0-15. The match clears the
// counter a cycle later, so an envelope step takes one cycle more than this: 9, 32, 63, ...
// (the data sheet's times / 256).
constexpr uint16_t kRateCompare[16] = {8,   31,  62,   94,   148,  219,   266,   312,
                                       391, 976, 1953, 3125, 3906, 11719, 19531, 31250};
// W4: cycles of TEST until the first cells of the noise register have drifted to one, and
// from one step of that drift to the next.
constexpr uint32_t kNoiseFadeStart[2] = {35000, 2519864};
constexpr uint32_t kNoiseFadeStep[2] = {1000, 315000};
// W8: cycles until a floating waveform DAC input loses its first bits, and per step after.
constexpr uint32_t kFloatingStart[2] = {182000, 4400000};
constexpr uint32_t kFloatingStep[2] = {1500, 50000};
// X3: cycles until the last byte on the data bus has faded.
constexpr uint32_t kBusTtl[2] = {0x1D00, 0xA2000};
// W3/W10: the noise register bits behind waveform output bits 11..4.
constexpr int kNoiseTap[8] = {20, 18, 14, 11, 9, 5, 2, 0};

// ---- combined waveforms (W10) -----------------------------------------------------------
//
// Selecting more than one of triangle, sawtooth and pulse shorts their sources together on
// the twelve DAC input lines. The model is that circuit as a network of conductances:
//
//   line W(n)  --gs--  D(n)        sawtooth: accumulator bit n (gsTop for bit 11)
//   line W(n)  --gt--  D(n-1)      triangle: accumulator bit n-1; W(0) --gtLow-- ground
//   line W(n)  --gp--  P(n)        pulse: a tap of the pulse line
//   P(n) --gl-- P(n+1), P(11) --gFeed-- supply       the pulse line, fed at the top end
//   D(k) --gHigh-- supply (bit set)  or  --gLow-- ground (bit clear)
//
// A driver pulls up weakly and down strongly, so a clear bit wins and also drags the lines
// near it down: through its neighbors' drivers (sawtooth + triangle put D(n) on two lines)
// and through the pulse line. A line reads 1 above the threshold. All conductances are
// relative to the sawtooth switch (gs = 1), voltages to the supply.
//
// The values are fitted so that the upper 8 bits equal what was sampled from real chips
// through OSC3 for all 4096 accumulator values (tools/siddiff --waves scores them); the
// circuit, not a table, decides every bit, including the low four that OSC3 cannot show.
struct WaveNet {
    double gLow, gHigh;   // a driver's pull to ground / to the supply
    double gt, gtLow;     // triangle switch; line 0 to ground
    double gtUpper;       // 8580: triangle switches of lines 11-4
    double gsTop;         // sawtooth switch of line 11
    double gp, gl, gFeed; // pulse switch, pulse line segment, the line's feed
    double threshold;
};
// [chip][combination]; 0 = not part of that combination's circuit.
constexpr WaveNet kWaveNet[2][4] = {
    {
     {1.4709, 0.0415058, 0.937336, 1.98729, 0.937336, 1.16369, 0, 0, 0, 0.336462},   // ST
     {21.0333, 0.103829, 1, 31.9139, 1, 0, 1.02409, 13.3887, 0.485742, 0.420355},   // PT
     {2.20441, 0.590311, 0, 0, 0, 1096.63, 0.767794, 15.92, 1.07649, 0.789634},   // PS
     {4.09184, 0.0261213, 0.778112, 1.14991, 0.778112, 0.321844, 0.550316, 29.6968, 1.34472, 0.54279},   // PST
    },
    {
     {0.877857, 0.0103743, 0.425589, 0.604121, 0.613909, 0.571147, 0, 0, 0, 0.157904},   // ST
     {105.487, 0.0832156, 1, 2.14399, 1.19217, 0, 2.31849, 4.92373, 0.383194, 0.309318},   // PT
     {2.76336, 0.0326999, 0, 0, 0, 0.702409, 1.04312, 2.44159, 0.150155, 0.277722},   // PS
     {0.151016, 0.00999101, 0.163315, 0.221118, 0.314586, 1.47723, 0.0989549, 0.0775693, 0.0418464, 0.434977},   // PST
    },
};
} // namespace

struct WaveTables {
    std::array<std::array<uint16_t, 4096>, 4> wave;   // ST, PT, PS, PST
    double margin = 1.0;   // the smallest distance of a line voltage from the threshold
};

namespace {

// Node 3n = D(n), 3n+1 = W(n), 3n+2 = P(n): every connection stays within 4 places, so the
// node equations are a band matrix (symmetric, diagonally dominant: no pivoting needed).
constexpr int kNodes = 36, kBand = 4;

WaveTables buildWaveTables(SidModel model) {
    WaveTables t;
    for (int c = 0; c < 4; ++c) {
        const unsigned wf = c == 0 ? 3 : c == 1 ? 5 : c == 2 ? 6 : 7;
        const WaveNet& p = kWaveNet[static_cast<int>(model)][c];
        const bool saw = wf & 2, tri = wf & 1, pulse = wf & 4;
        double base[kNodes][2 * kBand + 1] = {};
        double baseRhs[kNodes] = {};
        bool used[kNodes] = {};
        auto link = [&](int a, int b, double g) {
            base[a][kBand] += g; base[b][kBand] += g;
            base[a][kBand + b - a] -= g; base[b][kBand + a - b] -= g;
        };
        for (int n = 0; n < 12; ++n) used[3 * n + 1] = true;
        if (saw) for (int n = 0; n < 12; ++n) { link(3 * n + 1, 3 * n, n == 11 ? p.gsTop : 1.0); used[3 * n] = true; }
        if (tri) {
            for (int n = 1; n < 12; ++n) {
                link(3 * n + 1, 3 * (n - 1), (model == SidModel::Mos8580 && n >= 4) ? p.gtUpper : p.gt);
                used[3 * (n - 1)] = true;
            }
            base[1][kBand] += p.gtLow;
        }
        if (pulse) {
            for (int n = 0; n < 12; ++n) { link(3 * n + 1, 3 * n + 2, p.gp); used[3 * n + 2] = true; }
            for (int n = 0; n < 11; ++n) link(3 * n + 2, 3 * n + 5, p.gl);
            base[35][kBand] += p.gFeed;
            baseRhs[35] += p.gFeed;
        }
        for (int i = 0; i < kNodes; ++i) if (!used[i]) base[i][kBand] = 1;

        for (unsigned x = 0; x < 4096; ++x) {
            unsigned bits = x;
            // The triangle alone folds: with the top bit set the inverted bits are selected.
            // With the sawtooth also selected there is no fold, and on the 6581 the top
            // bit's line is held low (the chips' samples do not depend on it).
            if (tri && !saw && (x & 0x800)) bits = ~x;
            if (tri && saw && model == SidModel::Mos6581) bits &= 0x7FF;
            double a[kNodes][2 * kBand + 1], r[kNodes];
            for (int i = 0; i < kNodes; ++i) {
                for (int j = 0; j <= 2 * kBand; ++j) a[i][j] = base[i][j];
                r[i] = baseRhs[i];
            }
            for (int k = 0; k < 12; ++k) {
                if (!used[3 * k]) continue;
                if (bits >> k & 1) { a[3 * k][kBand] += p.gHigh; r[3 * k] += p.gHigh; }
                else a[3 * k][kBand] += p.gLow;
            }
            for (int i = 0; i < kNodes; ++i) {
                for (int k = i + 1; k <= i + kBand && k < kNodes; ++k) {
                    const double f = a[k][kBand + i - k] / a[i][kBand];
                    if (f == 0) continue;
                    for (int j = i; j <= i + kBand && j < kNodes; ++j) a[k][kBand + j - k] -= f * a[i][kBand + j - i];
                    r[k] -= f * r[i];
                }
            }
            uint16_t out = 0;
            for (int i = kNodes - 1; i >= 0; --i) {
                double v = r[i];
                for (int j = i + 1; j <= i + kBand && j < kNodes; ++j) v -= a[i][kBand + j - i] * r[j];
                r[i] = v / a[i][kBand];
                if (i % 3 == 1) {
                    const double d = r[i] - p.threshold;
                    if (d > 0) out = static_cast<uint16_t>(out | (1u << (i / 3)));
                    if (std::fabs(d) < t.margin) t.margin = std::fabs(d);
                }
            }
            t.wave[c][x] = out;
        }
    }
    return t;
}

const WaveTables& waveTables(SidModel model) {
    static const WaveTables tables[2] = {buildWaveTables(SidModel::Mos6581), buildWaveTables(SidModel::Mos8580)};
    return tables[static_cast<int>(model)];
}
} // namespace

// ---- DACs (D1, D2) ------------------------------------------------------------------------
//
// Both DACs are R-2R ladders: every bit drives its node through 2R, neighboring nodes are
// joined by R, the output is the top bit's node. The 8580's ladder is as in the textbook
// (2R/R = 2, the bottom node terminated with 2R to ground). The 6581's has no termination
// and 2R/R is about 2.2, so its steps are uneven. A bit that is off still passes a little
// (the switch transistor leaks).
namespace {
struct Ladder { double ratio; bool terminated; double leak; };
constexpr Ladder kLadder[2] = {{2.20, false, 0.0075}, {2.00, true, 0.0035}};

template <int Bits>
std::array<uint16_t, 1 << Bits> buildDac(const Ladder& l) {
    // The output for one bit alone at 1 V: the node equations of the ladder are tridiagonal.
    double weight[Bits];
    const double g2 = 1.0 / l.ratio;   // the 2R leg, with R = 1
    for (int bit = 0; bit < Bits; ++bit) {
        double diag[Bits], rhs[Bits];
        for (int i = 0; i < Bits; ++i) {
            diag[i] = g2 + (i > 0 ? 1.0 : 0.0) + (i < Bits - 1 ? 1.0 : 0.0) + (i == 0 && l.terminated ? g2 : 0.0);
            rhs[i] = i == bit ? g2 : 0.0;
        }
        for (int i = 1; i < Bits; ++i) {   // eliminate the -1 below the diagonal
            const double f = -1.0 / diag[i - 1];
            diag[i] += f;                  // minus f times the -1 above the diagonal
            rhs[i] -= f * rhs[i - 1];
        }
        weight[bit] = rhs[Bits - 1] / diag[Bits - 1];
    }
    std::array<uint16_t, 1 << Bits> table{};
    for (unsigned code = 0; code < (1u << Bits); ++code) {
        double v = 0;
        for (int bit = 0; bit < Bits; ++bit) v += ((code >> bit & 1) ? 1.0 : l.leak) * weight[bit];
        table[code] = static_cast<uint16_t>(((1 << Bits) - 1) * v + 0.5);
    }
    return table;
}

struct DacTables {
    std::array<uint16_t, 4096> wave;
    std::array<uint16_t, 256> envelope;
};
const DacTables& dacTables(SidModel model) {
    static const DacTables tables[2] = {{buildDac<12>(kLadder[0]), buildDac<8>(kLadder[0])},
                                        {buildDac<12>(kLadder[1]), buildDac<8>(kLadder[1])}};
    return tables[static_cast<int>(model)];
}
// D2: the waveform code at which a voice's output equals its level with the envelope at 0.
constexpr int kWaveZero[2] = {0x380, 0x9E0};
} // namespace

uint16_t Sid::waveDac(SidModel model, unsigned code) { return dacTables(model).wave[code & 0xFFF]; }
uint16_t Sid::envelopeDac(SidModel model, unsigned code) { return dacTables(model).envelope[code & 0xFF]; }

int Sid::voiceOutput(int voice) const {
    const Voice& v = s_.voice[voice];
    const DacTables& d = dacTables(model_);
    return (static_cast<int>(d.wave[v.out]) - kWaveZero[static_cast<int>(model_)]) * static_cast<int>(d.envelope[v.env]);
}

uint16_t Sid::combinedWave(SidModel model, unsigned waveform, unsigned index) {
    const int c = (waveform & 7) == 3 ? 0 : (waveform & 7) == 5 ? 1 : (waveform & 7) == 6 ? 2 : 3;
    return waveTables(model).wave[c][index & 0xFFF];
}

double Sid::combinedWaveMargin(SidModel model) { return waveTables(model).margin; }

Sid::Sid(SidModel model) : model_(model), tables_(&waveTables(model)) { reset(); }

void Sid::setModel(SidModel model) {
    model_ = model;
    tables_ = &waveTables(model);
}

void Sid::powerOn() {
    s_ = State{};
    reset();
}

void Sid::reset() {
    for (Voice& v : s_.voice) {
        const Voice before = v;
        v = Voice{};
        v.acc = before.acc;                 // not changed by reset (W4)
        v.triSawPipeline = before.triSawPipeline;
        v.env = before.env;                 // nor is the envelope counter (E7)
        v.envRead = before.envRead;
        setNoiseOut(v);
    }
    for (uint8_t& r : s_.reg) r = 0;
    s_.bus = 0;
    s_.busTtl = 0;
    // (The filter's capacitors keep their charge: RES is a digital pin. The paddle
    // converters run on: nothing in the sources says RES reaches them.)
}

// ---- envelope (E1-E7) ----------------------------------------------------------------

void Sid::setExpPeriod(Voice& v) {
    switch (v.env) {
    case 0xFF: v.expPeriod = 1; break;
    case 0x5D: v.expPeriod = 2; break;
    case 0x36: v.expPeriod = 4; break;
    case 0x1A: v.expPeriod = 8; break;
    case 0x0E: v.expPeriod = 16; break;
    case 0x06: v.expPeriod = 30; break;
    case 0x00:
        v.expPeriod = 1;
        v.holdZero = 1;   // E4: held at zero until the gate opens again
        break;
    default: break;
    }
}

// E3: a gate change takes effect in steps over the next cycles.
void Sid::stepStateChange(Voice& v) {
    --v.statePipeline;
    if (v.nextState == Attack) {
        if (v.statePipeline == 0) {
            v.state = Attack;
            v.rateCompare = kRateCompare[v.attack];
            v.holdZero = 0;
        }
    } else if ((v.state == Attack && v.statePipeline == 0) || (v.state == DecaySustain && v.statePipeline == 1)) {
        v.state = Release;
        v.rateCompare = kRateCompare[v.release];
    }
}

void Sid::clockEnvelope(Voice& v) {
    v.envRead = v.env;   // E8
    if (v.statePipeline) stepStateChange(v);

    if (v.envPipeline && --v.envPipeline == 0 && !v.holdZero) {   // the counter steps
        if (v.state == Attack) {
            v.env = static_cast<uint8_t>(v.env + 1);
            if (v.env == 0xFF) {
                v.state = DecaySustain;
                v.rateCompare = kRateCompare[v.decay];
            }
        } else {
            v.env = static_cast<uint8_t>(v.env - 1);
        }
        setExpPeriod(v);
    }

    if (v.expPipeline && --v.expPipeline == 0) {   // E2: the exponential counter's match
        v.expCounter = 0;
        if ((v.state == DecaySustain && v.env != v.sustain * 0x11) || v.state == Release) v.envPipeline = 1;
    } else if (v.rateMatch) {   // E1: the rate counter's match, a cycle after it happened
        v.rate = 0;
        v.rateMatch = 0;
        if (v.state == Attack) {
            v.expCounter = 0;
            v.envPipeline = 2;
        } else if (!v.holdZero) {
            v.expCounter = static_cast<uint8_t>(v.expCounter + 1);
            if (v.expCounter == v.expPeriod) v.expPipeline = v.expPeriod != 1 ? 2 : 1;
        }
    }

    // E1: 15 bits; past $7FFF the counter goes on from 1.
    if (v.rate != v.rateCompare) {
        v.rate = static_cast<uint16_t>(v.rate + 1);
        if (v.rate & 0x8000) v.rate = static_cast<uint16_t>((v.rate + 1) & 0x7FFF);
    } else {
        v.rateMatch = 1;
    }
}

// ---- oscillator and noise (W1-W4) ------------------------------------------------------

void Sid::setNoiseOut(Voice& v) {
    uint16_t out = 0;
    for (int i = 0; i < 8; ++i)
        if (v.noise & (1u << kNoiseTap[i])) out = static_cast<uint16_t>(out | (0x800 >> i));
    v.noiseOut = out;
}

void Sid::clockNoise(Voice& v) {
    const uint32_t bit0 = ((v.noise >> 22) ^ (v.noise >> 17)) & 1;
    v.noise = ((v.noise << 1) | bit0) & 0x7FFFFF;
    setNoiseOut(v);
}

// W10: a combined waveform's output bits pull the register bits behind them low.
void Sid::noiseWriteBack(Voice& v, uint16_t lines) {
    for (int i = 0; i < 8; ++i)
        if (!(lines & (0x800 >> i))) v.noise &= ~(1u << kNoiseTap[i]);
    v.noiseOut = static_cast<uint16_t>(v.noiseOut & lines);
}

// W4: with TEST held the register's cells drift to one: bit 0, and every cell above a cell
// that is one (bit 0 included).
void Sid::fadeNoise(Voice& v) {
    v.noise |= 1;
    v.noise = (v.noise | (v.noise << 1)) & 0x7FFFFF;
    setNoiseOut(v);
    if (v.noise != 0x7FFFFF) v.noiseReset = kNoiseFadeStep[static_cast<int>(model_)];
}

void Sid::clockOscillator(Voice& v) {
    if (v.test) {
        if (v.noiseReset && --v.noiseReset == 0) fadeNoise(v);
        v.pulseOut = 0xFFF;
        return;   // the accumulator is held; msbRising keeps its last value, as on the reference
    }
    const uint32_t next = (v.acc + v.freq) & 0xFFFFFF;
    const uint32_t rose = ~v.acc & next;
    v.acc = next;
    v.msbRising = (rose & 0x800000) ? 1 : 0;
    if (rose & 0x080000) v.shiftPipeline = 2;   // W3: the shift comes two cycles later
    else if (v.shiftPipeline && --v.shiftPipeline == 0) clockNoise(v);
}

// The triangle/sawtooth/pulse part of the output for the upper 12 accumulator bits.
uint16_t Sid::waveValue(const Voice& v, unsigned index) const {
    const uint16_t saw = static_cast<uint16_t>(index & 0xFFF);
    const uint16_t tri = static_cast<uint16_t>(((index & 0x800) ? ~index << 1 : index << 1) & 0xFFE);
    switch (v.waveform & 7) {
    case 1: return tri;
    case 2: return saw;
    case 0: case 4: return 0xFFF;   // none of the three, or pulse alone
    // W10: the sources shorted on the lines (the pulse level is applied by the caller).
    default: return tables_->wave[(v.waveform & 7) == 3 ? 0 : (v.waveform & 7) == 5 ? 1 : (v.waveform & 7) == 6 ? 2 : 3][index];
    }
}

// The DAC input lines for the selected waveform; wave = its triangle/sawtooth/pulse part.
uint16_t Sid::lineValue(const Voice& v, const Voice& source, uint16_t* wave) const {
    // W5: with ring modulation (and no sawtooth) the triangle's fold bit is the MSB
    // XOR NOT the sync source's MSB.
    const uint32_t ringMask = (v.ring && !(v.waveform & 2)) ? 0x800000u : 0u;
    const unsigned index = ((v.acc ^ (~source.acc & ringMask)) >> 12) & 0xFFF;
    const uint16_t noPulse = (v.waveform & 4) ? 0 : 0xFFF;
    const uint16_t noiseMask = (v.waveform & 8) ? v.noiseOut : 0xFFF;
    *wave = waveValue(v, index);
    uint16_t out = static_cast<uint16_t>(*wave & (noPulse | v.pulseOut) & noiseMask);
    if ((v.waveform & 0xC) == 0xC) {   // noise + pulse (W10)
        if (model_ == SidModel::Mos6581) out = out < 0xF00 ? 0 : static_cast<uint16_t>(out & (out << 1) & (out << 2));
        else out = out < 0xFC0 ? static_cast<uint16_t>(out & (out << 1)) : 0xFC0;
    }
    return out;
}

void Sid::formOutput(Voice& v, const Voice& source) {
    if (v.waveform) {
        const uint16_t noPulse = (v.waveform & 4) ? 0 : 0xFFF;
        const uint16_t noiseMask = (v.waveform & 8) ? v.noiseOut : 0xFFF;
        uint16_t wave = 0;
        const uint16_t out = lineValue(v, source, &wave);
        v.out = out;
        if ((v.waveform & 3) && model_ == SidModel::Mos8580) {   // W9
            v.osc = static_cast<uint16_t>(v.triSawPipeline & (noPulse | v.pulseOut) & noiseMask);
            v.triSawPipeline = wave;
        } else {
            v.osc = out;
        }
        // 6581: a combination with sawtooth pulls accumulator bit 23 low with output bit 11.
        if ((v.waveform & 2) && (v.waveform & 0xD) && model_ == SidModel::Mos6581)
            v.acc &= (static_cast<uint32_t>(out) << 12) | 0x7FFFFF;
        // W10: a combination's lines write into the noise register. W10a: on some 6581s
        // noise + pulse alone does not while the pulse level is high.
        const bool spared = !noisePulseDrains_ && model_ == SidModel::Mos6581 && v.waveform == 0xC && v.pulseOut;
        if (v.waveform > 8 && !v.test && v.shiftPipeline != 1 && !spared) noiseWriteBack(v, out);
    } else if (v.floatingTtl && --v.floatingTtl == 0) {
        // W8: the floating lines lose their charge; a bit goes when the bit above it is gone.
        v.out = v.osc = static_cast<uint16_t>(v.out & (v.out >> 1));
        if (v.out) v.floatingTtl = kFloatingStep[static_cast<int>(model_)];
    }
    v.pulseOut = ((v.acc >> 12) >= v.pw) ? 0xFFF : 0;   // W7: used in the next cycle
}

// ---- the chip (X1-X5) ------------------------------------------------------------------

void Sid::clock() {
    for (Voice& v : s_.voice) clockEnvelope(v);
    for (Voice& v : s_.voice) clockOscillator(v);
    // W2: hard sync after all three have counted. Voice n is synced by voice n-1 (1 by 3).
    for (int i = 0; i < 3; ++i) {
        Voice& src = s_.voice[i];
        Voice& dest = s_.voice[(i + 1) % 3];
        const Voice& srcSource = s_.voice[(i + 2) % 3];
        if (src.msbRising && dest.sync && !(src.sync && srcSource.msbRising)) dest.acc = 0;
    }
    for (int i = 0; i < 3; ++i) formOutput(s_.voice[i], s_.voice[(i + 2) % 3]);
    if (s_.busTtl && --s_.busTtl == 0) s_.bus = 0;
    clockPots();
}

// X5: a conversion takes 512 cycles. For the first 256 the chip holds both pins low and
// the capacitors are empty; then it lets go and counts the cycles, 0 to 255, while each
// capacitor charges through whatever is connected. The count at which a pin passes the
// input's threshold is captured into its register; a pin that has not passed by the end
// of the count captures 255.
void Sid::clockPots() {
    const unsigned phase = s_.potPhase;
    s_.potPhase = static_cast<uint16_t>((phase + 1) & 511);
    if (phase < 256) {
        if (phase == 0) s_.potLevel[0] = s_.potLevel[1] = s_.potCaptured[0] = s_.potCaptured[1] = 0;
        return;
    }
    const unsigned count = phase - 256;
    for (int pin = 0; pin < 2; ++pin) {
        if (s_.potCaptured[pin]) continue;
        s_.potLevel[pin] += potCharge_[pin];
        if (s_.potLevel[pin] >= (1u << 24) || count == 255) {
            s_.pot[pin] = static_cast<uint8_t>(count);
            s_.potCaptured[pin] = 1;
        }
    }
}

uint8_t Sid::read(unsigned reg) {
    const uint32_t ttl = kBusTtl[static_cast<int>(model_)];
    switch (reg & 0x1F) {
    case 0x19: case 0x1A: s_.bus = s_.pot[reg & 1 ? 0 : 1]; s_.busTtl = ttl; break;   // X5
    case 0x1B: s_.bus = static_cast<uint8_t>(s_.voice[2].osc >> 4); s_.busTtl = ttl; break;
    case 0x1C: s_.bus = s_.voice[2].envRead; s_.busTtl = ttl; break;
    default: break;   // write-only: the bus value (X3)
    }
    return s_.bus;
}

void Sid::writeControl(Voice& v, const Voice& source, uint8_t control) {
    const int m = static_cast<int>(model_);
    const uint8_t prevWave = v.waveform, prevTest = v.test;
    v.waveform = control >> 4;
    v.test = control & 0x08;
    v.ring = control & 0x04;
    v.sync = control & 0x02;
    if (!prevTest && v.test) {   // W4
        v.acc = 0;
        v.shiftPipeline = 0;
        v.noiseReset = kNoiseFadeStart[m];
        v.pulseOut = 0xFFF;
    } else if (prevTest && !v.test) {
        // The shift that TEST held half-way completes. While TEST was set every cell's
        // output, shorted to its waveform line at the taps, sat on the next cell's latch;
        // what the latches hold at this moment depends on which sources let go of the
        // lines and which arrive with this very write (W4b: the table the suite's
        // write-back programs give for each chip).
        const bool held = prevWave > 8 && (prevWave & 3);   // triangle or sawtooth held the lines
        if (model_ == SidModel::Mos6581) {
            if (held && (prevWave & v.waveform & 3)) {
                noiseWriteBack(v, v.out);                    // the same source goes on holding them
            } else if (held && (prevWave & 4) && (v.waveform & 4)) {
                // Let go, with the pulse line pulling up from the top bit's end: the upper
                // lines are back up when the latches close, the lowest three are not.
                noiseWriteBack(v, static_cast<uint16_t>(v.out | 0xF80));
            }
        } else if (held) {
            // The lines are still down, unless everything but noise is deselected; even
            // then when triangle and sawtooth both held them (W4a).
            if (v.waveform != 8 || (prevWave & 3) == 3) noiseWriteBack(v, v.out);
        } else if (prevWave == 0xC) {
            // From noise + pulse: triangle and sawtooth arriving together pull the lines
            // down in time; one of them alone (with noise only, or with noise + pulse)
            // leaves the old lines to be latched; anything else latches nothing.
            if ((v.waveform & 3) == 3) {
                uint16_t wave = 0;
                noiseWriteBack(v, lineValue(v, source, &wave));
            } else if (v.waveform == 0x9 || v.waveform == 0xE) {
                noiseWriteBack(v, v.out);
            }
        }
        const uint32_t bit0 = (~v.noise >> 17) & 1;
        v.noise = ((v.noise << 1) | bit0) & 0x7FFFFF;
        setNoiseOut(v);
    }
    if (v.waveform) formOutput(v, source);
    else if (prevWave) v.floatingTtl = kFloatingStart[m];

    // E3: the gate. The change of state is spread over the next cycles (stepStateChange).
    const uint8_t gate = control & 0x01;
    if (!v.gate && gate) {
        v.nextState = Attack;
        v.state = DecaySustain;   // for the first cycle the decay rate is the one selected
        v.rateCompare = kRateCompare[v.decay];
        v.statePipeline = 2;
        if (v.rateMatch || v.expPipeline == 2) v.envPipeline = (v.expPeriod == 1 || v.expPipeline == 2) ? 2 : 4;
        else if (v.expPipeline == 1) v.statePipeline = 3;
    } else if (v.gate && !gate) {
        v.nextState = Release;
        v.statePipeline = v.envPipeline ? 3 : 2;
    }
    v.gate = gate;
}

void Sid::write(unsigned reg, uint8_t value) {
    reg &= 0x1F;
    s_.bus = value;
    s_.busTtl = kBusTtl[static_cast<int>(model_)];
    s_.reg[reg] = value;
    if (reg >= 0x15) return;   // filter and volume: with the analog part
    Voice& v = s_.voice[reg / 7];
    const Voice& source = s_.voice[(reg / 7 + 2) % 3];
    switch (reg % 7) {
    case 0: v.freq = static_cast<uint16_t>((v.freq & 0xFF00) | value); break;
    case 1: v.freq = static_cast<uint16_t>((v.freq & 0x00FF) | (value << 8)); break;
    case 2:
        v.pw = static_cast<uint16_t>((v.pw & 0xF00) | value);
        v.pulseOut = ((v.acc >> 12) >= v.pw) ? 0xFFF : 0;
        break;
    case 3:
        v.pw = static_cast<uint16_t>((v.pw & 0x0FF) | ((value & 0x0F) << 8));
        v.pulseOut = ((v.acc >> 12) >= v.pw) ? 0xFFF : 0;
        break;
    case 4: writeControl(v, source, value); break;
    case 5:   // E6
        v.attack = value >> 4;
        v.decay = value & 0x0F;
        if (v.state == Attack) v.rateCompare = kRateCompare[v.attack];
        else if (v.state == DecaySustain) v.rateCompare = kRateCompare[v.decay];
        break;
    default:
        v.sustain = value >> 4;
        v.release = value & 0x0F;
        if (v.state == Release) v.rateCompare = kRateCompare[v.release];
        break;
    }
}

} // namespace k64
