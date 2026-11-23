// audiotest: checks the audio output path's two parts with numbers only (nothing is played).
//
// The rate conversion (system clock rate -> 48 kHz), on the PAL and the NTSC clock:
//   - a constant comes out as the same constant;
//   - one second of input gives 48000 samples, within one;
//   - tones in the pass band (100 Hz .. 19 kHz) keep their level within 0.05 dB, and what
//     comes out besides the tone itself (noise from a coarsely placed filter, images) is
//     at least 90 dB below it;
//   - tones above the stop band (30 kHz .. 400 kHz), which a careless conversion folds back
//     into the audible band, come out at least 90 dB down;
//   - a step arrives after half the filter's length, within one output sample.
// The board's output network: 3 dB down at 16 kHz and at 16 Hz, flat in between, and no
// constant level passes.
// Exit status 1 with a line per failed check.
#include "audio.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
int failures = 0;
void check(bool ok, const char* what, double got, double low, double high) {
    if (ok) return;
    std::printf("FAIL: %s: %.6g, allowed %.6g .. %.6g\n", what, got, low, high);
    ++failures;
}

// Level in dB of a sine of hz after the conversion (input amplitude 1), from 0.25 s of it.
// With residue: the level of everything else in the output, after the best-fitting sine of
// that frequency has been taken away.
double toneLevel(k64::Resampler& r, double inputHz, double hz, double* residue = nullptr) {
    r.reset();
    const size_t n = static_cast<size_t>(inputHz / 4);
    std::vector<float> out;
    float buf[256];
    for (size_t i = 0; i < n; ++i) {
        r.push(static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / inputHz)));
        while (size_t got = r.read(buf, 256)) out.insert(out.end(), buf, buf + got);
    }
    double sq = 0.0;
    size_t count = 0;
    for (size_t i = 1000; i < out.size(); ++i, ++count) sq += static_cast<double>(out[i]) * out[i];   // past the start-up
    const double rms = std::sqrt(sq / static_cast<double>(count));
    if (residue) {
        // Least squares for a*sin + b*cos at the tone's frequency, then what is left.
        double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
        for (size_t i = 1000; i < out.size(); ++i) {
            const double w = 2.0 * kPi * hz * static_cast<double>(i) / 48000.0;
            const double sn = std::sin(w), cs = std::cos(w);
            ss += sn * sn; cc += cs * cs; sc += sn * cs; ys += out[i] * sn; yc += out[i] * cs;
        }
        const double det = ss * cc - sc * sc;
        const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
        double left = 0.0;
        for (size_t i = 1000; i < out.size(); ++i) {
            const double w = 2.0 * kPi * hz * static_cast<double>(i) / 48000.0;
            const double d = out[i] - a * std::sin(w) - b * std::cos(w);
            left += d * d;
        }
        const double leftRms = std::sqrt(left / static_cast<double>(count));
        *residue = leftRms > 0.0 ? 20.0 * std::log10(leftRms * std::sqrt(2.0)) : -300.0;
    }
    return rms > 0.0 ? 20.0 * std::log10(rms * std::sqrt(2.0)) : -300.0;
}

// Level in dB of a sine of hz after the board filter, measured after it has settled.
double boardLevel(double clockHz, double hz) {
    k64::BoardFilter f;
    f.configure(clockHz);
    const double seconds = hz < 100.0 ? 3.0 : 0.3;
    const size_t n = static_cast<size_t>(clockHz * seconds);
    double sq = 0.0;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        const double y = f.clock(std::sin(2.0 * kPi * hz * static_cast<double>(i) / clockHz));
        if (i >= n / 2) { sq += y * y; ++count; }
    }
    return 20.0 * std::log10(std::sqrt(sq / static_cast<double>(count)) * std::sqrt(2.0));
}
} // namespace

int main() {
    for (double clockHz : {985248.6, 1022727.1}) {
        char what[160];
        k64::Resampler r;
        r.configure(clockHz, 48000.0);
        float buf[512];

        // A constant, and the number of samples per second.
        size_t produced = 0;
        float last = 0.0f;
        for (size_t i = 0; i < static_cast<size_t>(clockHz + 0.5); ++i) {
            r.push(0.5f);
            while (size_t got = r.read(buf, 512)) { produced += got; last = buf[got - 1]; }
        }
        std::snprintf(what, sizeof what, "clock %.0f: a constant 0.5 comes out as", clockHz);
        check(std::fabs(last - 0.5f) < 1e-5f, what, last, 0.49999, 0.50001);
        std::snprintf(what, sizeof what, "clock %.0f: samples from one second of input", clockHz);
        check(std::fabs(static_cast<double>(produced) - 48000.0) <= 1.0, what, static_cast<double>(produced), 47999, 48001);

        for (double hz : {100.0, 1000.0, 5000.0, 10000.0, 15000.0, 19000.0}) {
            double residue = 0.0;
            const double db = toneLevel(r, clockHz, hz, &residue);
            std::snprintf(what, sizeof what, "clock %.0f: level of a %.0f Hz tone (dB)", clockHz, hz);
            check(std::fabs(db) < 0.05, what, db, -0.05, 0.05);
            std::snprintf(what, sizeof what, "clock %.0f: everything but the tone, for a %.0f Hz tone (dB)", clockHz, hz);
            check(residue < -90.0, what, residue, -300, -90);
        }
        for (double hz : {30000.0, 47000.0, 49000.0, 95000.0, 200000.0, 400000.0}) {
            const double db = toneLevel(r, clockHz, hz);
            std::snprintf(what, sizeof what, "clock %.0f: what is left of a %.0f Hz tone (dB)", clockHz, hz);
            check(db < -90.0, what, db, -300, -90);
        }

        // A step from 0 to 1 at input sample 20000: the output passes 0.5 at that moment
        // plus half the filter's length.
        r.reset();
        std::vector<float> out;
        for (size_t i = 0; i < 60000; ++i) {
            r.push(i >= 20000 ? 1.0f : 0.0f);
            while (size_t got = r.read(buf, 512)) out.insert(out.end(), buf, buf + got);
        }
        double crossing = -1.0;
        for (size_t i = 1; i < out.size(); ++i)
            if (out[i - 1] < 0.5f && out[i] >= 0.5f) { crossing = static_cast<double>(i - 1) + (0.5 - out[i - 1]) / (out[i] - out[i - 1]); break; }
        const double expected = (20000.0 - 0.5 + r.taps() / 2) / (clockHz / 48000.0);
        std::snprintf(what, sizeof what, "clock %.0f: output sample at which a step arrives", clockHz);
        check(std::fabs(crossing - expected) <= 1.0, what, crossing, expected - 1.0, expected + 1.0);

        // The board's output network.
        std::snprintf(what, sizeof what, "clock %.0f: board network at 1 kHz (dB)", clockHz);
        double db = boardLevel(clockHz, 1000.0);
        check(db > -0.1 && db < 0.0, what, db, -0.1, 0.0);
        std::snprintf(what, sizeof what, "clock %.0f: board network at 15915 Hz (dB)", clockHz);
        db = boardLevel(clockHz, 15915.0);
        check(db > -3.3 && db < -2.7, what, db, -3.3, -2.7);
        std::snprintf(what, sizeof what, "clock %.0f: board network at 15.915 Hz (dB)", clockHz);
        db = boardLevel(clockHz, 15.915);
        check(db > -3.3 && db < -2.7, what, db, -3.3, -2.7);
        k64::BoardFilter f;
        f.configure(clockHz);
        double y = 1.0;
        for (size_t i = 0; i < static_cast<size_t>(clockHz); ++i) y = f.clock(1.0);   // one second of a constant
        std::snprintf(what, sizeof what, "clock %.0f: board network, a constant after one second", clockHz);
        check(std::fabs(y) < 1e-6, what, y, -1e-6, 1e-6);
    }
    std::printf(failures ? "audiotest: %d failure(s)\n" : "audiotest: ok\n", failures);
    return failures ? 1 : 0;
}
