#include "sid.h"

namespace k64 {

namespace {
// E1: cycles per envelope step for the rate values 0-15 (the data sheet's times / 256,
// one more than rounded, as measured on the chip).
constexpr uint16_t kRatePeriod[16] = {9,   32,  63,   95,   149,  220,   267,   313,
                                      392, 977, 1954, 3126, 3907, 11720, 19532, 31251};
// W4: cycles of TEST until the noise register's cells have drifted to all ones.
constexpr uint32_t kNoiseReset[2] = {0x8000, 0x950000};
// W8: cycles until a floating waveform DAC input reads 0.
constexpr uint32_t kFloatingTtl[2] = {200000, 5000000};
// X3: cycles until the last byte on the data bus has faded.
constexpr uint32_t kBusTtl[2] = {0x1D00, 0xA2000};
// W3/W10: the noise register bits behind waveform output bits 11..4.
constexpr int kNoiseTap[8] = {20, 18, 14, 11, 9, 5, 2, 0};
} // namespace

Sid::Sid(SidModel model) : model_(model) { reset(); }

void Sid::setModel(SidModel model) { model_ = model; }

void Sid::reset() {
    for (Voice& v : s_.voice) {
        const uint32_t acc = v.acc;
        const uint16_t pipeline = v.triSawPipeline;
        v = Voice{};
        v.acc = acc;               // not changed by reset (W4)
        v.triSawPipeline = pipeline;
        setNoiseOut(v);
    }
    for (uint8_t& r : s_.reg) r = 0;
    s_.bus = 0;
    s_.busTtl = 0;
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

void Sid::clockEnvelope(Voice& v) {
    if (v.envPipeline) {   // E5: the decrement decided a cycle ago
        v.env = static_cast<uint8_t>(v.env - 1);
        v.envPipeline = 0;
        setExpPeriod(v);
    }
    // E1: 15 bits; past $7FFF the counter goes on from 1.
    v.rate = static_cast<uint16_t>(v.rate + 1);
    if (v.rate & 0x8000) v.rate = static_cast<uint16_t>((v.rate + 1) & 0x7FFF);
    if (v.rate != v.ratePeriod) return;
    v.rate = 0;

    if (v.state != Attack) {   // E2
        v.expCounter = static_cast<uint8_t>(v.expCounter + 1);
        if (v.expCounter != v.expPeriod) return;
    }
    v.expCounter = 0;
    if (v.holdZero) return;
    switch (v.state) {
    case Attack:
        v.env = static_cast<uint8_t>(v.env + 1);
        if (v.env == 0xFF) {
            v.state = DecaySustain;
            v.ratePeriod = kRatePeriod[v.decay];
        }
        break;
    case DecaySustain:
        if (v.env == v.sustain * 0x11) return;   // E3: both nibbles compared
        if (v.expPeriod != 1) {
            v.envPipeline = 1;
            return;
        }
        v.env = static_cast<uint8_t>(v.env - 1);
        break;
    default:   // Release
        if (v.expPeriod != 1) {
            v.envPipeline = 1;
            return;
        }
        v.env = static_cast<uint8_t>(v.env - 1);
        break;
    }
    setExpPeriod(v);
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
void Sid::noiseWriteBack(Voice& v) {
    for (int i = 0; i < 8; ++i)
        if (!(v.out & (0x800 >> i))) v.noise &= ~(1u << kNoiseTap[i]);
    v.noiseOut = static_cast<uint16_t>(v.noiseOut & v.out);
}

void Sid::clockOscillator(Voice& v) {
    if (v.test) {
        if (v.noiseReset && --v.noiseReset == 0) {
            v.noise = 0x7FFFFF;
            setNoiseOut(v);
        }
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
    // W10, PROVISIONAL: the combinations of triangle, sawtooth and pulse short the
    // sources together on the chip; until the model for that is settled they are the
    // plain AND of their parts (the pulse level is applied by the caller).
    case 3: return tri & saw;
    case 5: return tri;
    case 6: return saw;
    case 7: return tri & saw;
    default: return 0xFFF;   // none of the three, or pulse alone
    }
}

void Sid::formOutput(Voice& v, const Voice& source) {
    if (v.waveform) {
        // W5: with ring modulation (and no sawtooth) the triangle's fold bit is the MSB
        // XOR NOT the sync source's MSB.
        const uint32_t ringMask = (v.ring && !(v.waveform & 2)) ? 0x800000u : 0u;
        const unsigned index = ((v.acc ^ (~source.acc & ringMask)) >> 12) & 0xFFF;
        const uint16_t noPulse = (v.waveform & 4) ? 0 : 0xFFF;
        const uint16_t noiseMask = (v.waveform & 8) ? v.noiseOut : 0xFFF;
        const uint16_t wave = waveValue(v, index);
        uint16_t out = static_cast<uint16_t>(wave & (noPulse | v.pulseOut) & noiseMask);
        if ((v.waveform & 0xC) == 0xC) {   // noise + pulse (W10)
            if (model_ == SidModel::Mos6581) out = out < 0xF00 ? 0 : static_cast<uint16_t>(out & (out << 1) & (out << 2));
            else out = out < 0xFC0 ? static_cast<uint16_t>(out & (out << 1)) : 0xFC0;
        }
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
        if (v.waveform > 8 && !v.test && v.shiftPipeline != 1) noiseWriteBack(v);
    } else if (v.floatingTtl && --v.floatingTtl == 0) {
        v.out = v.osc = 0;   // W8
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
}

uint8_t Sid::read(unsigned reg) {
    const uint32_t ttl = kBusTtl[static_cast<int>(model_)];
    switch (reg & 0x1F) {
    case 0x19: case 0x1A: s_.bus = 0xFF; s_.busTtl = ttl; break;   // X5: nothing connected
    case 0x1B: s_.bus = static_cast<uint8_t>(s_.voice[2].osc >> 4); s_.busTtl = ttl; break;
    case 0x1C: s_.bus = s_.voice[2].env; s_.busTtl = ttl; break;
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
        v.noiseReset = kNoiseReset[m];
        v.pulseOut = 0xFFF;
    } else if (prevTest && !v.test) {
        // The shift that TEST held half-way completes; a combination with noise selected
        // until now may first write its output back.
        bool writeBack = prevWave > 8 && v.waveform != 8;
        if (prevWave == 0xC && (model_ == SidModel::Mos6581 || (v.waveform != 0x9 && v.waveform != 0xE))) writeBack = false;
        if (model_ == SidModel::Mos6581 &&
            (((prevWave & 3) == 1 && (v.waveform & 3) == 2) || ((prevWave & 3) == 2 && (v.waveform & 3) == 1)))
            writeBack = false;
        if (writeBack) noiseWriteBack(v);
        const uint32_t bit0 = (~v.noise >> 17) & 1;
        v.noise = ((v.noise << 1) | bit0) & 0x7FFFFF;
        setNoiseOut(v);
    }
    if (v.waveform) formOutput(v, source);
    else if (prevWave) v.floatingTtl = kFloatingTtl[m];

    // E3: the gate.
    const uint8_t gate = control & 0x01;
    if (!v.gate && gate) {
        v.state = Attack;
        v.ratePeriod = kRatePeriod[v.attack];
        v.holdZero = 0;
        v.envPipeline = 0;
    } else if (v.gate && !gate) {
        v.state = Release;
        v.ratePeriod = kRatePeriod[v.release];
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
        if (v.state == Attack) v.ratePeriod = kRatePeriod[v.attack];
        else if (v.state == DecaySustain) v.ratePeriod = kRatePeriod[v.decay];
        break;
    default:
        v.sustain = value >> 4;
        v.release = value & 0x0F;
        if (v.state == Release) v.ratePeriod = kRatePeriod[v.release];
        break;
    }
}

} // namespace k64
