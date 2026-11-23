// MOS 6581 / 8580 SID: the digital part (oscillators, noise, envelopes, the readable
// registers). Filter and output stage follow.
//
// Every rule and its source: local-notes/hw/sid.md (W = waveform generator, E = envelope,
// X = chip level). One clock() per system cycle.
#pragma once

#include <cstdint>

namespace k64 {

enum class SidModel : uint8_t { Mos6581, Mos8580 };

class Sid {
public:
    explicit Sid(SidModel model = SidModel::Mos6581);

    void setModel(SidModel model);
    SidModel model() const { return model_; }
    void reset();   // the RES pin; the oscillators' accumulators keep their value (W4)

    void clock();
    uint8_t read(unsigned reg);            // reg = address & $1F
    void write(unsigned reg, uint8_t v);

    enum EnvState : uint8_t { Attack, DecaySustain, Release };

#pragma pack(push, 1)
    struct Voice {
        // Waveform generator.
        uint32_t acc = 0x555555;     // 24-bit accumulator; even bits set at power on
        uint16_t freq = 0, pw = 0;
        uint8_t waveform = 0;        // control register bits 7-4
        uint8_t test = 0, ring = 0, sync = 0, gate = 0;
        uint8_t msbRising = 0;       // accumulator bit 23 went high in this cycle
        uint32_t noise = 0x7FFFFE;   // 23-bit shift register
        uint32_t noiseReset = 0;     // cycles until TEST has pulled the register to all ones
        uint8_t shiftPipeline = 0;   // cycles until a pending shift happens
        uint16_t noiseOut = 0;       // the register's eight taps as output bits 11-4
        uint16_t pulseOut = 0xFFF;   // pulse level for the next output ($FFF or 0)
        uint16_t out = 0;            // 12-bit waveform output (DAC input)
        uint16_t osc = 0;            // what OSC3 shows of it
        uint16_t triSawPipeline = 0x555;   // 8580: triangle/sawtooth one cycle late
        uint32_t floatingTtl = 0;    // cycles until a floating DAC input has faded
        // Envelope generator.
        uint16_t rate = 0, ratePeriod = 9;
        uint8_t expCounter = 0, expPeriod = 1;
        uint8_t env = 0;             // the envelope counter
        uint8_t envPipeline = 0;     // a decrement decided in the last cycle
        uint8_t holdZero = 1;
        uint8_t attack = 0, decay = 0, sustain = 0, release = 0;
        uint8_t state = Release;
    };
    struct State {
        Voice voice[3];
        uint8_t reg[0x20] = {};      // registers as written
        uint8_t bus = 0;             // last byte on the chip's data bus
        uint32_t busTtl = 0;         // cycles until it has faded
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    void setState(const State& s) { s_ = s; }

private:
    void clockEnvelope(Voice& v);
    void setExpPeriod(Voice& v);
    void clockOscillator(Voice& v);
    void clockNoise(Voice& v);
    void setNoiseOut(Voice& v);
    void noiseWriteBack(Voice& v);
    void formOutput(Voice& v, const Voice& source);
    uint16_t waveValue(const Voice& v, unsigned index) const;
    void writeControl(Voice& v, const Voice& source, uint8_t control);

    SidModel model_;
    State s_;
};

} // namespace k64
