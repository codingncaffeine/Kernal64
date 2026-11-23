// The SID's analog part: filter, mixer and volume stage (hw/sid.md section F).
//
// The chip's circuit, from the published die analysis, solved with the textbook FET law:
//
//   voices --+--> summer --> Vhp --> integrator --> Vbp --> integrator --> Vlp
//            |       ^                                |                     |
//            |       +---- resonance stage <----------+                     |
//            |       +------------------------------------------------------+
//            +--> mixer (voices not routed to the filter + the selected outputs) --> volume
//
// Every "resistor" is a FET in triode mode with its gate at the supply, so its current is
// K/2 ((Vg - Vt - Vs)^2 - (Vg - Vt - Vd)^2): linear in F(V) = (Vdd - Vt - V)^2. An inverting
// stage with inputs v_k through relative conductances n_k is then ONE equation in the
// op-amp's input voltage x:
//
//   sum n_k F(v_k) - (N + 1) F(x) + F(T(x)) = 0,   N = sum n_k,   output = T(x)
//
// T is the op-amp's transfer function: on the 6581 an NMOS inverter pair, computed here
// from its four transistors; on the 8580 a real op-amp against a reference voltage. An
// integrator's capacitor sits between the op-amp's input and output, so its voltage vc
// fixes x through T(x) - x = vc; each cycle it takes the charge that flows in from the
// stage before it: through the cutoff FET (and, on the 6581, a fixed long FET beside it).
//
// For speed every stage is a table over the conductance-weighted mean of F of its inputs,
// built once from the equations above. The tables use no math-library function whose
// last bit may differ between platforms: the capacitor voltages are part of a save state.
#include "sid.h"

#include <cmath>
#include <memory>
#include <vector>

namespace k64 {

namespace {

// ---- exp and log by their series: + - * / only (std::sqrt, frexp and ldexp are exact) ----
constexpr double kLn2 = 0.69314718055994530942;

double detExp(double x) {
    if (x < -700.0) return 0.0;
    if (x > 700.0) x = 700.0;
    const double k = std::floor(x / kLn2 + 0.5);
    const double r = x - k * kLn2;   // |r| <= ln2 / 2
    double sum = 1.0, term = 1.0;
    for (int i = 1; i <= 18; ++i) {
        term *= r / i;
        sum += term;
    }
    return std::ldexp(sum, static_cast<int>(k));
}

double detLog(double x) {   // x > 0
    int e = 0;
    double m = std::frexp(x, &e);   // m in [0.5, 1)
    if (m < 0.70710678118654752440) { m *= 2.0; --e; }
    const double z = (m - 1.0) / (m + 1.0), z2 = z * z;   // log m = 2 atanh z, |z| < 0.172
    double sum = 0.0, power = z;
    for (int i = 1; i <= 41; i += 2) {
        sum += power / i;
        power *= z2;
    }
    return 2.0 * sum + e * kLn2;
}

// Current from drain to source of an NMOS FET: the quadratic law in its symmetric form,
// which covers triode and saturation and both directions.
inline double fet(double k, double vg, double vd, double vs, double vt) {
    const double a = vg - vs - vt, b = vg - vd - vt;
    return 0.5 * k * ((a > 0 ? a * a : 0.0) - (b > 0 ? b * b : 0.0));
}

// A function tabulated over [low, high], read with linear interpolation.
struct Curve {
    double low = 0.0, perUnit = 0.0;
    std::vector<double> y;
    void shape(double lo, double hi, int points) {
        low = lo;
        perUnit = (points - 1) / (hi - lo);
        y.assign(static_cast<size_t>(points), 0.0);
    }
    double x(int i) const { return low + i / perUnit; }
    double operator()(double v) const {
        const double p = (v - low) * perUnit;
        if (p <= 0.0) return y.front();
        const size_t last = y.size() - 1;
        if (p >= static_cast<double>(last)) return y.back();
        const size_t i = static_cast<size_t>(p);
        return y[i] + (y[i + 1] - y[i]) * (p - static_cast<double>(i));
    }
};

struct ChipConstants {
    double vdd, vth;              // supply; threshold of the "resistor" FETs
    double voiceDc, voiceRange;   // a voice: its level at rest and the span of its 20-bit value
    double cap;                   // the integrators' capacitors
    double uCox;
    double mixerN;                // mixer: an input's conductance over the feedback's
    double filterMix;             // the filter outputs' mixer inputs, relative to a voice's
    double volumeDiv;             // volume stage: n = volume / this
    double outSpan;               // volts that count as full scale of the audio output
};
// 6581: measured on a 6581R4AR; 8580: from its die and data. (hw/sid.md F lists the sources.)
constexpr ChipConstants kChip[2] = {
    {12.18, 1.31, 5.075, 1.5, 470e-12, 20e-6, 8.0 / 6.0, 0.93, 12.0, 2.0},
    {9.09, 0.80, 4.7975, 0.24, 22e-9, 100e-6, 8.0 / 5.0, 1.0, 16.0, 0.7},
};

constexpr int kPoints = 2049;

struct Analog {
    const ChipConstants& c;
    bool old;                      // 6581
    double vddt, fMax, bias;
    Curve amp;                     // the op-amp: output from input
    Curve inputOf;                 // integrator: the op-amp's input from the capacitor's voltage
    Curve resonance[16], summer[4], mixer[4][4], volume[16];
    Curve ekv;                     // 6581: the cutoff FET's current term
    std::vector<double> cutoff;    // 6581: F of the cutoff DAC's voltage per FC; 8580: W/L per FC

    double F(double v) const { const double d = vddt - v; return d > 0 ? d * d : 0.0; }

    explicit Analog(SidModel model) : c(kChip[static_cast<int>(model)]), old(model == SidModel::Mos6581) {
        vddt = c.vdd - c.vth;
        fMax = vddt * vddt;
        buildAmp();
        // Where an op-amp with its output tied to its input settles.
        double lo = 0, hi = c.vdd;
        for (int i = 0; i < 80; ++i) { const double m = 0.5 * (lo + hi); if (amp(m) > m) lo = m; else hi = m; }
        bias = 0.5 * (lo + hi);

        // T(x) - x falls with x: the capacitor voltage decides the input voltage.
        inputOf.shape(amp(c.vdd) - c.vdd, amp(0.0), 1 << 14);
        for (size_t i = 0; i < inputOf.y.size(); ++i) {
            const double vc = inputOf.x(static_cast<int>(i));
            lo = 0; hi = c.vdd;
            for (int it = 0; it < 60; ++it) { const double m = 0.5 * (lo + hi); if (amp(m) - m > vc) lo = m; else hi = m; }
            inputOf.y[i] = 0.5 * (lo + hi);
        }

        for (int r = 0; r < 16; ++r) buildStage(resonance[r], resonanceN(r));
        for (int n = 0; n < 4; ++n) buildStage(summer[n], 2.0 + n);   // resonance stage + low-pass + n voices
        for (int v = 0; v < 4; ++v)
            for (int f = 0; f < 4; ++f) buildStage(mixer[v][f], c.mixerN * (v + c.filterMix * f));
        for (int v = 0; v < 16; ++v) buildStage(volume[v], v / c.volumeDiv);
        buildCutoff();
    }

    // An inverting stage whose inputs' conductances add up to n (relative to the feedback
    // FET's): its output for every weighted mean u of F over the inputs.
    void buildStage(Curve& t, double n) {
        t.shape(0.0, fMax, kPoints);
        for (int i = 0; i < kPoints; ++i) {
            if (n <= 0.0) { t.y[static_cast<size_t>(i)] = bias; continue; }
            const double a = n * t.x(i);
            double lo = 0, hi = c.vdd;   // a - (n + 1) F(x) + F(T(x)) rises with x
            for (int it = 0; it < 60; ++it) {
                const double m = 0.5 * (lo + hi);
                if (a - (n + 1.0) * F(m) + F(amp(m)) > 0) hi = m; else lo = m;
            }
            t.y[static_cast<size_t>(i)] = amp(0.5 * (lo + hi));
        }
    }

    double resonanceN(int res) const {
        if (old) return (~res & 15) / 8.0;   // four switched input FETs, 1/Q about ~RES / 8
        // 8580: RES bits 0-1 switch a FET across the feedback, bits 2-3 pick the input FET.
        static constexpr double across[4] = {0.0, 15.3, 7.3, 4.7}, input[4] = {1.0, 1.4, 2.0, 2.8};
        const double feedback = (res & 3) ? 1.4 * across[res & 3] / (1.4 + across[res & 3]) : 1.4;
        return feedback / input[res >> 2];
    }

    void buildAmp() {
        amp.shape(0.0, c.vdd, 1 << 14);
        if (old) {
            // Four transistors: M1 a source follower (gate = the input, source = node A), M2
            // pulls A down with its gate on the OUTPUT, M4 (gate = A) drives the output
            // against the saturated load M3. Strengths relative to M1 and M3; fitted to the
            // measured transfer curve (0.09 V RMS between 0.8 and 10.3 V).
            const double r1 = 0.02534, r2 = 10.72998, vt = 1.12979, vtLoad = 1.91472;
            for (size_t n = 0; n < amp.y.size(); ++n) {
                const double vi = amp.x(static_cast<int>(n));
                double lo = 0, hi = c.vdd;
                for (int it = 0; it < 60; ++it) {
                    const double vo = 0.5 * (lo + hi);
                    double aLo = 0, aHi = c.vdd;
                    for (int jt = 0; jt < 60; ++jt) {
                        const double a = 0.5 * (aLo + aHi);
                        if (fet(1.0, vi, c.vdd, a, vt) - fet(r1, vo, a, 0.0, vt) > 0) aLo = a; else aHi = a;
                    }
                    const double a = 0.5 * (aLo + aHi);
                    if (fet(1.0, c.vdd, c.vdd, vo, vtLoad) - fet(r2, a, vo, 0.0, vt) > 0) lo = vo; else hi = vo;
                }
                amp.y[n] = 0.5 * (lo + hi);
            }
        } else {
            // A real op-amp against the 4.75 V divider: gain about -380 between its rails.
            const double high = 8.91, low = 1.30, center = 4.8125, steep = 100.7;
            for (size_t n = 0; n < amp.y.size(); ++n) {
                const double e = detExp(2.0 * steep * (amp.x(static_cast<int>(n)) - center));
                amp.y[n] = 0.5 * (high + low) - 0.5 * (high - low) * (e - 1.0) / (e + 1.0);   // tanh
            }
        }
    }

    void buildCutoff() {
        cutoff.assign(2048, 0.0);
        if (!old) {
            // The FC bits switch FETs in parallel; with none of them a half-size one is on.
            for (int fc = 0; fc < 2048; ++fc) cutoff[static_cast<size_t>(fc)] = 0.003075 * (fc ? 2.0 * fc : 1.0);
            return;
        }
        // The cutoff DAC: an 11-bit R-2R ladder without termination (2R/R about 2.2), from
        // 6.65 V up over 2.63 V; stored as F of its voltage.
        double weight[11];
        const double g2 = 1.0 / 2.2;
        for (int bit = 0; bit < 11; ++bit) {
            double diag[11], rhs[11];
            for (int i = 0; i < 11; ++i) {
                diag[i] = g2 + (i > 0 ? 1.0 : 0.0) + (i < 10 ? 1.0 : 0.0);
                rhs[i] = i == bit ? g2 : 0.0;
            }
            for (int i = 1; i < 11; ++i) {
                const double f = -1.0 / diag[i - 1];
                diag[i] += f;
                rhs[i] -= f * rhs[i - 1];
            }
            weight[bit] = rhs[10] / diag[10];
        }
        for (int fc = 0; fc < 2048; ++fc) {
            double v = 0;
            for (int bit = 0; bit < 11; ++bit) v += ((fc >> bit & 1) ? 1.0 : 0.0075) * weight[bit];
            cutoff[static_cast<size_t>(fc)] = F(6.65 + 2047.0 * v / 2048.0 * 2.63);
        }
        // The cutoff FET runs through sub-threshold, triode and saturation; the EKV law's
        // term ln^2(1 + e^(z / 2Ut)) for z = gate - threshold - terminal voltage.
        const double ut2 = 2.0 * 26.0e-3;
        ekv.shape(-2.0, 13.0, 1 << 15);
        for (size_t i = 0; i < ekv.y.size(); ++i) {
            const double l = detLog(1.0 + detExp(ekv.x(static_cast<int>(i)) / ut2));
            ekv.y[i] = l * l;
        }
    }

    // One cycle of an integrator: the capacitor takes the current that flows from vi into
    // the op-amp's input node. Returns the op-amp's output.
    double integrate(double vi, double& vc, int fc, double perCycle) const {
        const double x = inputOf(vc);
        double i;
        if (old) {
            const double snake = fet(c.uCox / 115.0, c.vdd, vi, x, c.vth);   // the fixed long FET
            // The cutoff FET's gate sits half-way (in F) between vi and the DAC's voltage.
            const double gate = vddt - std::sqrt((F(vi) + cutoff[static_cast<size_t>(fc)]) / 2.0) - c.vth;
            const double is = 2.0 * c.uCox * (26.0e-3 * 26.0e-3) * 9.0;
            i = snake + is * (ekv(gate - x) - ekv(gate - vi));
        } else {
            i = fet(c.uCox * cutoff[static_cast<size_t>(fc)], 4.7975 * 1.6, vi, x, c.vth);
        }
        vc -= i * perCycle / c.cap;
        if (vc < inputOf.low) vc = inputOf.low;
        const double top = inputOf.x(static_cast<int>(inputOf.y.size()) - 1);
        if (vc > top) vc = top;
        return inputOf(vc) + vc;
    }
};

const Analog& analog(SidModel model) {
    if (model == SidModel::Mos6581) {
        static const Analog a(SidModel::Mos6581);
        return a;
    }
    static const Analog a(SidModel::Mos8580);
    return a;
}

} // namespace

double Sid::clockFilter(int voice1, int voice2, int voice3) {
    const Analog& a = analog(model_);
    const ChipConstants& c = a.c;
    const int fc = (s_.reg[0x16] << 3) | (s_.reg[0x15] & 7);
    const int res = s_.reg[0x17] >> 4, routed = s_.reg[0x17] & 7;   // EXT IN (bit 3): nothing is connected
    const int mode = s_.reg[0x18] >> 4, volume = s_.reg[0x18] & 15;
    const int voice[3] = {voice1, voice2, voice3};
    double f[3];
    for (int k = 0; k < 3; ++k) f[k] = a.F(c.voiceDc + voice[k] * (c.voiceRange / 1048576.0));

    // The state is kept relative to the level everything rests at, so that all zeros is
    // the chip at rest.
    double vhp = s_.filterHp + a.bias;
    const double vbpOld = a.inputOf(s_.filterBp) + s_.filterBp;
    const double vlp = a.integrate(vbpOld, s_.filterLp, fc, cyclePeriod_);
    const double vbp = a.integrate(vhp, s_.filterBp, fc, cyclePeriod_);

    // Summer: the band-pass through the resonance stage, the low-pass, the routed voices.
    double sum = a.F(a.resonance[res](a.F(vbp))) + a.F(vlp);
    int count = 0;
    for (int k = 0; k < 3; ++k)
        if (routed >> k & 1) { sum += f[k]; ++count; }
    vhp = a.summer[count](sum / (2.0 + count));
    s_.filterHp = vhp - a.bias;

    // Mixer: the voices that bypass the filter (voice 3 unless switched off: X4) and the
    // filter outputs selected by the mode bits.
    double direct = 0.0, filtered = 0.0;
    int voices = 0, outputs = 0;
    for (int k = 0; k < 3; ++k)
        if (!(routed >> k & 1) && !(k == 2 && (mode & 8))) { direct += f[k]; ++voices; }
    if (mode & 1) { filtered += a.F(vlp); ++outputs; }
    if (mode & 2) { filtered += a.F(vbp); ++outputs; }
    if (mode & 4) { filtered += a.F(vhp); ++outputs; }
    const double weight = voices + c.filterMix * outputs;
    const double mixed = weight > 0.0 ? a.mixer[voices][outputs]((direct + c.filterMix * filtered) / weight) : a.bias;
    const double out = a.volume[volume](a.F(mixed));
    return (out - a.bias) / c.outSpan;
}

double Sid::audioFullScaleVolts(SidModel model) { return kChip[static_cast<int>(model)].outSpan; }

double Sid::clockAudio() { return clockFilter(voiceOutput(0), voiceOutput(1), voiceOutput(2)); }

} // namespace k64
