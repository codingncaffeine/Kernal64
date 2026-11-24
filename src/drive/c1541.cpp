#include "c1541.h"

#include <algorithm>
#include <cstring>

namespace k64 {

C1541::C1541() { reset(); }

void C1541::setRoms(const RomSet& roms) {
    romsOk_ = roms.has(RomRole::Drive1541Low) && roms.has(RomRole::Drive1541High);
    if (!romsOk_) return;
    std::copy_n(roms.get(RomRole::Drive1541Low).begin(), 8192, rom_.begin());
    std::copy_n(roms.get(RomRole::Drive1541High).begin(), 8192, rom_.begin() + 8192);
}

void C1541::reset() {
    for (size_t i = 0; i < ram_.size(); ++i) ram_[i] = (i & 0x40) ? 0xFF : 0x00;
    via1_.reset();
    via2_.reset();
    cpu_.reset();
    cycle_ = 0;
    m_ = Mech{};
    bus_last_ = 0;
    via1_.setCa1(atnLow_);
}

// The address decoder (hw/drive.md B1): the upper half of the address space is the ROM,
// twice; the lower half repeats every $2000: RAM at $0000-$07FF, the serial bus's 6522
// at $1800-$1BFF, the disk's at $1C00-$1FFF, nothing in between.
uint8_t C1541::read(uint16_t a) {
    uint8_t v;
    if (a & 0x8000) v = rom_[a & 0x3FFF];
    else switch (a & 0x1C00) {
    case 0x0000: case 0x0400: v = ram_[a & 0x07FF]; break;
    case 0x1800: v = via1_.read(a & 15); break;
    case 0x1C00: v = via2_.read(a & 15); break;
    default: v = bus_last_; break;   // B1: nothing answers; the bus keeps its last value
    }
    bus_last_ = v;
    return v;
}

void C1541::write(uint16_t a, uint8_t v) {
    bus_last_ = v;
    if (a & 0x8000) return;
    switch (a & 0x1C00) {
    case 0x0000: case 0x0400: ram_[a & 0x07FF] = v; break;
    case 0x1800: via1_.write(a & 15, v); break;
    case 0x1C00: via2_.write(a & 15, v); break;
    default: break;
    }
}

// ---- the serial bus (hw/drive.md B2) -----------------------------------------------
//
// Port B of the first 6522 through inverters: PB1 high pulls DATA, PB3 high pulls CLK;
// PB0, PB2 and PB7 read 1 while DATA, CLK and ATN are low. PB4 (ATNA) and the ATN line
// go through an exclusive OR onto DATA: the drive answers ATN without its CPU. A pin
// the chip does not drive floats high at the inverter.

bool C1541::pullsClk() const {
    return ((via1_.outB() | ~via1_.ddrB()) & 0x08) != 0;
}

bool C1541::pullsData() const {
    const uint8_t pins = static_cast<uint8_t>(via1_.outB() | ~via1_.ddrB());
    const bool atna = (pins & 0x10) != 0;
    return (pins & 0x02) != 0 || atna != atnLow_;
}

void C1541::setBus(bool atnLow, bool clkLow, bool dataLow) {
    atnLow_ = atnLow;
    clkLow_ = clkLow;
    dataLow_ = dataLow;
    via1_.setCa1(atnLow);   // ATN, inverted, is also the chip's CA1 input
}

uint8_t C1541::viaPortA(int via, uint8_t out, uint8_t ddr) {
    // The disk's 6522: port A's inputs are the read circuit's byte latch (R5).
    if (via == 1) return static_cast<uint8_t>((out & ddr) | (m_.latch & ~ddr));
    return static_cast<uint8_t>(out | ~ddr);
}

uint8_t C1541::viaPortB(int via, uint8_t out, uint8_t ddr) {
    uint8_t pins = static_cast<uint8_t>(out | ~ddr);
    if (via == 0) {
        pins &= 0x1A;                                         // PB1, PB3, PB4: the chip's own
        if (dataLow_ || pullsData()) pins |= 0x01;
        if (clkLow_ || pullsClk()) pins |= 0x04;
        pins |= static_cast<uint8_t>(jumpers_ << 5);          // PB5, PB6: the device number
        if (atnLow_) pins |= 0x80;
        return pins;
    }
    // The disk's 6522 (R6): PB7 low while the read circuit sees SYNC; PB4 low while the
    // write protect sensor is dark (a disk with its notch covered).
    pins = static_cast<uint8_t>(pins & ~(0x80 | 0x10) & 0xFF);
    pins = static_cast<uint8_t>(pins | ((out & ddr) & 0x90));
    if (!(ddr & 0x80) && !(readMode() && m_.shift == 0x3FF)) pins |= 0x80;
    if (!(ddr & 0x10) && !m_.wpDark && !(disk_ && disk_->writeProtected)) pins |= 0x10;
    return pins;
}

// PB3 and PB2 of the disk's 6522, through drivers: a pin the chip does not drive yet
// floats high, so both are on from power on until the ROM has set the port up.
bool C1541::led() const { return ((via2_.outB() | ~via2_.ddrB()) & 0x08) != 0; }
bool C1541::motor() const { return ((via2_.outB() | ~via2_.ddrB()) & 0x04) != 0; }

// ---- the disk mechanism (hw/drive.md R) -------------------------------------------

namespace {
constexpr uint32_t kTicksPerRev = 3'200'000;   // R1: 16 MHz at 300 rpm
}

// R2: the stepper's two lines count through four phases; one phase up moves the head a
// half-track in (to higher tracks), one down moves it out. The disk keeps turning under it.
void C1541::viaOutputs(int via) {
    if (via != 1) return;
    const uint8_t phase = static_cast<uint8_t>((via2_.outB() | ~via2_.ddrB()) & 3);
    const int step = (phase - m_.phase) & 3;
    if (step == 1 && m_.halfTrack < 2 + GcrDisk::kHalfTracks - 1) ++m_.halfTrack;
    else if (step == 3 && m_.halfTrack > 2) --m_.halfTrack;
    m_.phase = phase;
}

// One drive cycle = 16 ticks of the 16 MHz clock (R1-R4).
void C1541::mechanism() {
    m_.readyLow = 0;
    if (m_.wpDark) --m_.wpDark;
    const bool spinning = motor() && disk_ != nullptr;
    const std::vector<uint8_t>* track = nullptr;
    if (spinning) {
        const auto& t = disk_->tracks[static_cast<size_t>(GcrDisk::index(m_.halfTrack))];
        if (!t.empty()) track = &t;
    }
    const uint64_t bitsPerRev = track ? track->size() * 8 : 0;
    const uint8_t zone = static_cast<uint8_t>(((via2_.outB() | ~via2_.ddrB()) >> 5) & 3);
    for (int tick = 0; tick < 16; ++tick) {
        // A flux change (a 1 on the disk) restarts both counters (R3).
        if (m_.flux) {
            m_.flux = 0;
            m_.ue7 = zone;
            m_.uf4 = 0;
        }
        if (++m_.ue7 == 16) {
            m_.ue7 = zone;
            m_.uf4 = static_cast<uint8_t>((m_.uf4 + 1) & 15);
            if ((m_.uf4 & 3) == 2) {
                // A bit cell ends: a 1 if no flux change came for a whole cell since the last.
                m_.shift = static_cast<uint16_t>(((m_.shift << 1) | (m_.uf4 == 2 ? 1 : 0)) & 0x3FF);
                if (!readMode()) {
                    // W2: the write register's top bit goes onto the disk under the head.
                    if (spinning) writeBit((m_.wbuf & 0x80) != 0, zone);
                    m_.wbuf = static_cast<uint8_t>(m_.wbuf << 1);
                } else {
                    m_.writing = 0;
                }
                m_.byteDone = 0;
                if (readMode() && m_.shift == 0x3FF) {
                    m_.bits = 0;   // SYNC holds the bit counter at its start (R4)
                } else if (++m_.bits == 8) {
                    m_.bits = 0;
                    m_.byteDone = 1;
                    if (readMode()) m_.latch = static_cast<uint8_t>(m_.shift);
                }
            } else if ((m_.uf4 & 3) == 3 && m_.byteDone) {
                // R4: BYTE READY is low while the counter stands at 7 and the bit-cell
                // counter's QA and QB are both high: the zone-clock period after the shift
                // that completed the byte. The same line loads the write register from port
                // A's lines (W1): the DOS's byte in write mode, the read latch otherwise.
                m_.wbuf = viaPortA(1, via2_.outA(), via2_.ddrA());
                if (soe()) m_.readyLow = 1;
            }
        }
        if (!spinning) continue;
        // The disk moves one tick; a new bit cell under the head that holds a 1 is a flux change
        // (not while the head writes: its read amplifier is off, W3).
        const uint32_t next = m_.angle + 1 == kTicksPerRev ? 0 : m_.angle + 1;
        if (track && readMode()) {
            const uint64_t a = m_.angle * bitsPerRev / kTicksPerRev;
            const uint64_t b = next ? next * bitsPerRev / kTicksPerRev : 0;
            if (a != b && ((*track)[b >> 3] & (0x80 >> (b & 7)))) m_.flux = 1;
        }
        m_.angle = next;
    }
}

// W2: one bit at the cell under the head. A half-track with nothing recorded gets a track of
// the length the zone being written gives in one revolution, blank (no flux changes).
void C1541::writeBit(bool one, uint8_t zone) {
    if (disk_->writeProtected) return;
    const size_t i = static_cast<size_t>(GcrDisk::index(m_.halfTrack));
    auto& t = disk_->tracks[i];
    if (t.empty()) {
        t.assign(static_cast<size_t>(zoneTrackBytes(zone)), 0x00);
        disk_->zone[i] = zone;
    }
    // W2: a write pass starts at the bit under the head and puts each cell on the next bit,
    // whatever the track's recorded length: the zone clock's cells per revolution are not a
    // whole number, and mapping each cell from the angle would write some bits twice.
    const uint64_t bits = t.size() * 8;
    if (!m_.writing) m_.wpos = static_cast<uint32_t>(static_cast<uint64_t>(m_.angle) * bits / kTicksPerRev);
    m_.writing = 1;
    const uint64_t b = m_.wpos;
    m_.wpos = static_cast<uint32_t>((m_.wpos + 1) % bits);
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (b & 7));
    t[b >> 3] = static_cast<uint8_t>(one ? (t[b >> 3] | mask) : (t[b >> 3] & ~mask));
    disk_->written[i] = true;
    disk_->changed = true;
}

// ---- save states ------------------------------------------------------------------

size_t C1541::stateSize() {
    return 2048 + sizeof(uint64_t) + 3 + 2 * sizeof(Via6522::State) + sizeof(Cpu6502<Bus>::Snapshot) + sizeof(Mech) + 1 +
           1 + static_cast<size_t>(GcrDisk::kHalfTracks) * (4 + kSurfaceTrackBytes);
}

void C1541::saveState(uint8_t* out) const {
    auto put = [&out](const void* p, size_t n) { std::memcpy(out, p, n); out += n; };
    put(ram_.data(), ram_.size());
    put(&cycle_, sizeof cycle_);
    const uint8_t lines[3] = {atnLow_, clkLow_, dataLow_};
    put(lines, 3);
    put(&via1_.state(), sizeof(Via6522::State));
    put(&via2_.state(), sizeof(Via6522::State));
    const auto snap = cpu_.snapshot();
    put(&snap, sizeof snap);
    put(&m_, sizeof m_);
    put(&bus_last_, 1);
    // The disk's surface: per half-track its length, zone, written flag, then its slot.
    const uint8_t present = disk_ ? 1 : 0;
    put(&present, 1);
    for (size_t i = 0; i < static_cast<size_t>(GcrDisk::kHalfTracks); ++i) {
        const std::vector<uint8_t>* t = disk_ ? &disk_->tracks[i] : nullptr;
        const bool kept = t && t->size() <= kSurfaceTrackBytes;
        const uint16_t len = !t ? 0 : kept ? static_cast<uint16_t>(t->size()) : kTrackNotKept;
        const uint8_t meta[4] = {static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8), disk_ ? disk_->zone[i] : uint8_t{0},
                                 static_cast<uint8_t>(disk_ && disk_->written[i])};
        put(meta, 4);
        if (kept && !t->empty()) std::memcpy(out, t->data(), t->size());
        if (kept) std::memset(out + t->size(), 0, kSurfaceTrackBytes - t->size());
        else std::memset(out, 0, kSurfaceTrackBytes);
        out += kSurfaceTrackBytes;
    }
}

void C1541::loadState(const uint8_t* in) {
    auto get = [&in](void* p, size_t n) { std::memcpy(p, in, n); in += n; };
    get(ram_.data(), ram_.size());
    get(&cycle_, sizeof cycle_);
    uint8_t lines[3];
    get(lines, 3);
    atnLow_ = lines[0] != 0;
    clkLow_ = lines[1] != 0;
    dataLow_ = lines[2] != 0;
    Via6522::State v;
    get(&v, sizeof v);
    via1_.setState(v);
    get(&v, sizeof v);
    via2_.setState(v);
    Cpu6502<Bus>::Snapshot snap;
    get(&snap, sizeof snap);
    cpu_.restore(snap);
    get(&m_, sizeof m_);
    get(&bus_last_, 1);
    uint8_t present = 0;
    get(&present, 1);
    // A surface goes onto the disk in the drive (the frontend's disk control put the state's
    // disk there first). Restoring it marks the disk changed when it differs, so the kept copy
    // follows what is in the drive again.
    for (size_t i = 0; i < static_cast<size_t>(GcrDisk::kHalfTracks); ++i) {
        uint8_t meta[4];
        get(meta, 4);
        const uint16_t len = static_cast<uint16_t>(meta[0] | meta[1] << 8);
        if (present && disk_ && len != kTrackNotKept) {
            auto& t = disk_->tracks[i];
            if (t.size() != len || (len && std::memcmp(t.data(), in, len) != 0)) {
                t.assign(in, in + len);
                disk_->changed = true;
            }
            disk_->zone[i] = meta[2];
            disk_->written[i] = meta[3] != 0;
        }
        in += kSurfaceTrackBytes;
    }
}

void C1541::tick() {
    mechanism();
    via2_.setCa1(!m_.readyLow);
    cpu_.setSo(m_.readyLow != 0);
    cpu_.setIrq(via1_.irq() || via2_.irq());
    cpu_.tick();
    via1_.clock();
    via2_.clock();
    ++cycle_;
}

} // namespace k64
