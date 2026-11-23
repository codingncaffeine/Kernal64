// Audio output: the board's output network after the SID's audio pin, and the conversion
// from one value per system clock cycle to the frontend's sample rate.
//
// The conversion is a band-limited one (plan decision D8): every output sample is a
// windowed-sinc weighted sum of the input around its moment in time. Taking the nearest
// input value, or interpolating between two, folds everything above half the output rate
// back into the audible band, and the SID's waveforms have plenty up there.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace k64 {

// The C64 board between the SID's audio pin and the audio jack: a low-pass (10 kOhm,
// 1000 pF: 16 kHz) and the output capacitor against the load (10 uF, 1 kOhm: 16 Hz).
class BoardFilter {
public:
    void configure(double clockHz);
    void reset() { low_ = high_ = 0.0; }
    double clock(double in) {
        low_ += lowStep_ * (in - low_);
        high_ += highStep_ * (low_ - high_);
        return low_ - high_;
    }
    struct State { double low, high; };
    State state() const { return {low_, high_}; }
    void setState(const State& s) { low_ = s.low; high_ = s.high; }

private:
    double lowStep_ = 0.0, highStep_ = 0.0;
    double low_ = 0.0, high_ = 0.0;
};

class Resampler {
public:
    // Pass band up to passHz, nothing above stopHz (which may lie as far above half the
    // output rate as passHz lies below it: what folds back then lands above passHz).
    void configure(double inputHz, double outputHz, double passHz = 20000.0, double stopHz = 28000.0);
    void reset();   // forgets the input seen so far; the output position starts over

    void push(float sample) {
        ring_[head_] = ring_[head_ + kRing] = sample;   // twice: a window never wraps
        head_ = (head_ + 1) & (kRing - 1);
        ++pushed_;
        if (pushed_ > nextCenter_ + half_) produce();   // the window's last sample is in
    }
    size_t available() const { return out_.size(); }
    // Moves up to max samples to dst; returns how many.
    size_t read(float* dst, size_t max);

    int taps() const { return taps_; }

private:
    void produce();

    static constexpr int kRing = 2048;     // input values kept; a power of two above the taps
    static constexpr int kPhases = 32;     // kernel positions per input sample, interpolated between
    std::vector<float> ring_ = std::vector<float>(2 * kRing, 0.0f);
    std::vector<float> kernel_;            // [(kPhases + 1) * taps_]
    std::vector<float> out_;
    int taps_ = 0, half_ = 0;
    int head_ = 0;
    uint64_t pushed_ = 0;                  // input samples so far
    uint64_t nextCenter_ = 0;              // the input sample at or before the next output's moment
    uint64_t fraction_ = 0;                // and how far past it, in 1/2^32 of an input sample
    uint64_t step_ = 0;                    // input samples per output sample, 32.32
};

} // namespace k64
