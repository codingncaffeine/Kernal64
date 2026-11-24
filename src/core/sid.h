// MOS 6581 / 8580 SID: oscillators, noise, envelopes, the readable registers and the DACs
// (sid.cpp); filter, mixer and volume stage (sid_analog.cpp).
//
// Every rule and its source: local-notes/hw/sid.md (W = waveform generator, E = envelope,
// X = chip level). One clock() per system cycle.
#pragma once

#include <cstdint>

namespace k64 {

struct WaveTables;

enum class SidModel : uint8_t { Mos6581, Mos8580 };

class Sid {
public:
    explicit Sid(SidModel model = SidModel::Mos6581);

    void setModel(SidModel model);
    SidModel model() const { return model_; }
    void reset();   // the RES pin; accumulators and envelope counters keep their value (W4, E7)
    void powerOn(); // everything as after switching on; the settings (model, clock, below) stay

    // W10a, a way in which 6581s differ: with noise + pulse selected and the pulse level
    // high, most drain the noise register within a few cycles (the default); some leave
    // it alone.
    void setNoisePulseDrains(bool drains) { noisePulseDrains_ = drains; }
    bool noisePulseDrains() const { return noisePulseDrains_; }

    // X5, the two paddle converters (pin 0 = POTX, 1 = POTY): how fast what is connected
    // to the pin charges its capacitor, in units of 1/2^24 of the way to the input's
    // threshold per cycle; 0 = nothing connected. Conductances in parallel add.
    void setPotCharge(int pin, uint32_t perCycle) { potCharge_[pin & 1] = perCycle; }
    // The charge rate of a resistance that alone gives the register value 0 .. 255.
    static constexpr uint32_t potChargeFor(unsigned value) { return ((1u << 24) + value) / (value + 1); }

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
        uint16_t rate = 0, rateCompare = 8;
        uint8_t rateMatch = 0;       // the rate counter equaled its compare value a cycle ago
        uint8_t expCounter = 0, expPeriod = 1;
        uint8_t env = 0xAA;          // the envelope counter; odd bits set at power on
        uint8_t envRead = 0xAA;      // what ENV3 shows: the counter one cycle ago
        uint8_t envPipeline = 0;     // cycles until the counter steps
        uint8_t expPipeline = 0;     // cycles until the exponential counter's match takes effect
        uint8_t statePipeline = 0;   // cycles left of a state change started by the gate
        uint8_t holdZero = 0;
        uint8_t attack = 0, decay = 0, sustain = 0, release = 0;
        uint8_t state = Release, nextState = Release;
    };
    struct State {
        Voice voice[3];
        uint8_t reg[0x20] = {};      // registers as written
        uint8_t bus = 0;             // last byte on the chip's data bus
        uint32_t busTtl = 0;         // cycles until it has faded
        // Filter: the two integrators' capacitor voltages and the summer's output, the
        // latter relative to its level at rest (all zero = the chip at rest).
        double filterBp = 0.0, filterLp = 0.0, filterHp = 0.0;
        // X5: the paddle converters. Position in their 512-cycle period, the registers as
        // last captured, the capacitors' charge and whether this period has captured.
        uint16_t potPhase = 0;
        uint8_t pot[2] = {0xFF, 0xFF};
        uint8_t potCaptured[2] = {};
        uint32_t potLevel[2] = {};
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    void setState(const State& s) { s_ = s; }
    // In use for sound: the volume is above 0. Not the voices: a tune's player silences every voice
    // for a moment before each note (hard restart: waveform and envelope at 0), and between notes
    // the tune is still playing; the volume stays set while it plays.
    bool audible() const { return (s_.reg[0x18] & 0x0F) != 0; }

    // W10: the 12 output bits of a combination of triangle (1), sawtooth (2) and pulse (4)
    // for the upper 12 accumulator bits, pulse level high; and the smallest distance of any
    // line voltage of that model's combinations from the switching threshold (in units of
    // the supply voltage), for the tools.
    static uint16_t combinedWave(SidModel model, unsigned waveform, unsigned index);
    static double combinedWaveMargin(SidModel model);

    // D1: what the waveform DAC (12 bits) and the envelope DAC (8 bits) make of a code,
    // on the scale of the code (0 .. 4095, 0 .. 255).
    static uint16_t waveDac(SidModel model, unsigned code);
    static uint16_t envelopeDac(SidModel model, unsigned code);
    // D2: a voice's output: the waveform DAC's level around its zero level, times the
    // envelope DAC's level. About -2048*255 .. 2047*255.
    int voiceOutput(int voice) const;
    // F: one cycle of the analog part (filter, mixer, volume stage); returns the audio
    // output, about -1 .. 1 around the level the chip rests at. Called once per cycle when
    // audio is wanted. clockFilter takes the three voices' outputs from outside (tools).
    double clockAudio();
    double clockFilter(int voice1, int voice2, int voice3);
    // The filter's time constants are in seconds: it needs the length of a cycle.
    void setClockHz(double hz) { cyclePeriod_ = 1.0 / hz; }
    // How many volts at the chip's audio pin the output value 1.0 stands for (tools).
    static double audioFullScaleVolts(SidModel model);

private:
    void clockEnvelope(Voice& v);
    void stepStateChange(Voice& v);
    void fadeNoise(Voice& v);
    void setExpPeriod(Voice& v);
    void clockOscillator(Voice& v);
    void clockNoise(Voice& v);
    void setNoiseOut(Voice& v);
    void noiseWriteBack(Voice& v, uint16_t lines);
    uint16_t lineValue(const Voice& v, const Voice& source, uint16_t* wave) const;
    void formOutput(Voice& v, const Voice& source);
    uint16_t waveValue(const Voice& v, unsigned index) const;
    void writeControl(Voice& v, const Voice& source, uint8_t control);
    void clockPots();

    SidModel model_;
    const WaveTables* tables_;   // W10: the combined waveforms of this chip model
    double cyclePeriod_ = 1.0 / 985248.0;
    bool noisePulseDrains_ = true;
    uint32_t potCharge_[2] = {};   // inputs (the machine sets them): see setPotCharge
    State s_;
};

} // namespace k64
