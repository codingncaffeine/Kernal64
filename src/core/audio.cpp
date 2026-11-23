#include "audio.h"

#include <cmath>

namespace k64 {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Modified Bessel function of the first kind, order 0 (for the Kaiser window).
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 200; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// 1 - e^-x for a small positive x, by its series: the same bits on every platform (the
// filter's state is part of a save state, and a math library's exp() is not).
double oneMinusExp(double x) {
    double sum = 0.0, term = 1.0;
    for (int k = 1; k <= 24; ++k) {
        term *= -x / k;
        sum -= term;
    }
    return sum;
}
} // namespace

void BoardFilter::configure(double clockHz) {
    const double dt = 1.0 / clockHz;
    lowStep_ = oneMinusExp(dt / (10e3 * 1000e-12));
    highStep_ = oneMinusExp(dt / (1e3 * 10e-6));
    reset();
}

void Resampler::configure(double inputHz, double outputHz, double passHz, double stopHz) {
    // Kaiser window design: 96 dB down in the stop band (the 16-bit noise floor).
    const double attenuation = 96.0;
    const double beta = 0.1102 * (attenuation - 8.7);
    const double width = (stopHz - passHz) / inputHz;                // transition band, cycles per input sample
    int taps = static_cast<int>(std::ceil((attenuation - 7.95) / (14.36 * width))) | 1;
    if (taps > kRing - 2) taps = kRing - 3;
    taps_ = taps;
    half_ = taps / 2;
    const double cutoff = (passHz + stopHz) / 2.0 / inputHz;         // middle of the transition band
    const double i0Beta = besselI0(beta);

    // kernel_[p][j] weights the input sample (center - half + j) for an output moment
    // p / kPhases of a sample past the center.
    kernel_.assign(static_cast<size_t>(kPhases + 1) * static_cast<size_t>(taps_), 0.0f);
    for (int p = 0; p <= kPhases; ++p) {
        std::vector<double> row(static_cast<size_t>(taps_));
        double sum = 0.0;
        for (int j = 0; j < taps_; ++j) {
            const double t = (j - half_) - static_cast<double>(p) / kPhases;   // distance from the moment
            const double x = t / (half_ + 1.0);                                // -1 .. 1 across the window
            const double window = std::fabs(x) < 1.0 ? besselI0(beta * std::sqrt(1.0 - x * x)) / i0Beta : 0.0;
            const double arg = 2.0 * kPi * cutoff * t;
            const double sinc = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(arg) / arg;
            row[static_cast<size_t>(j)] = 2.0 * cutoff * sinc * window;
            sum += row[static_cast<size_t>(j)];
        }
        // Every position has the same gain for a constant input.
        for (int j = 0; j < taps_; ++j)
            kernel_[static_cast<size_t>(p) * static_cast<size_t>(taps_) + static_cast<size_t>(j)] = static_cast<float>(row[static_cast<size_t>(j)] / sum);
    }
    step_ = static_cast<uint64_t>(std::llround(inputHz / outputHz * 4294967296.0));
    reset();
}

void Resampler::reset() {
    for (float& v : ring_) v = 0.0f;
    out_.clear();
    // As if half a window of silence had already arrived: the first output comes with the
    // first input, and N inputs give N / step outputs from the start.
    head_ = half_;
    pushed_ = static_cast<uint64_t>(half_);
    nextCenter_ = 0;
    fraction_ = 0;
}

void Resampler::produce() {
    // The output's moment lies fraction_ past input sample nextCenter_; the window is the
    // samples nextCenter_ - half_ .. nextCenter_ + half_, the last of which just arrived.
    const double position = static_cast<double>(fraction_ >> 8) / 16777216.0 * kPhases;
    const int p = static_cast<int>(position);
    const float mix = static_cast<float>(position - p);
    const float* a = &kernel_[static_cast<size_t>(p) * static_cast<size_t>(taps_)];
    const float* b = a + taps_;
    // Sample number n sits at ring index (head_ - (pushed_ - n)) mod kRing.
    const uint64_t first = nextCenter_ - static_cast<uint64_t>(half_);   // may wrap below zero at the start: zeros there
    const int back = static_cast<int>(pushed_ - first);                  // how far behind head_ the window starts
    const float* x = &ring_[static_cast<size_t>((head_ - back) & (kRing - 1))];
    float lo = 0.0f, hi = 0.0f;
    for (int j = 0; j < taps_; ++j) {
        lo += x[j] * a[j];
        hi += x[j] * b[j];
    }
    out_.push_back(lo + (hi - lo) * mix);
    fraction_ += step_;
    nextCenter_ += fraction_ >> 32;
    fraction_ &= 0xFFFFFFFFull;
}

size_t Resampler::read(float* dst, size_t max) {
    const size_t n = out_.size() < max ? out_.size() : max;
    for (size_t i = 0; i < n; ++i) dst[i] = out_[i];
    out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

} // namespace k64
