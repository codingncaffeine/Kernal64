#include "folderdrive.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace k64 {

namespace {

// Bus timing in machine cycles (about microseconds), from the serial bus document: EOI after 200 us
// without the talker's clock, acknowledged by holding DATA for 60 us; a bit held at least 60 us for a
// C64 listener (its video chip can stop its processor for 40 us); the frame acknowledged within 1 ms.
constexpr uint32_t kEoiTimeout = 200, kEoiAck = 60;
// A LISTEN or TALK for another device: acknowledged for this many cycles, then off the bus.
constexpr uint32_t kIgnoreAck = 60;
// Both halves of a bit last 80 us: the C64 KERNAL watches CLK in loops of 15-30 cycles (a debounced double
// read), and its video chip can stop its processor for about 40 cycles on a badline, so a half shorter than
// a stop plus a loop can pass unseen and the C64 counts a bit short (measured: with 20 us the first byte of a
// LOAD was never seen; with 60 us a byte in the middle of a listing was lost).
constexpr uint32_t kBitSetup = 80, kBitValid = 80, kByteGap = 80, kAckTimeout = 1000;

// PETSCII file names <-> host file names. Upper-case PETSCII letters stay letters; shifted letters
// become lower case; what a host file name cannot hold becomes "_".
char petsciiToHost(uint8_t c) {
    if (c >= 0xC1 && c <= 0xDA) return static_cast<char>('a' + (c - 0xC1));
    if (c >= 0x20 && c <= 0x5D && !std::strchr("/\\:*?\"<>|", c)) return static_cast<char>(c);
    return '_';
}
uint8_t hostToPetscii(char c) {
    if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c - 'a' + 'A');
    if (c >= 0x20 && c <= 0x5D) return static_cast<uint8_t>(c);
    return '?';
}
std::string toPetsciiName(const std::string& host) {
    std::string out;
    for (char c : host) out += static_cast<char>(hostToPetscii(c));
    return out.substr(0, 16);
}
// A DOS pattern ("*" the rest, "?" one character) against a name, both PETSCII.
bool matches(const std::string& pattern, const std::string& name) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '*') return true;
        if (i >= name.size()) return false;
        if (pattern[i] != '?' && pattern[i] != name[i]) return false;
    }
    return pattern.size() == name.size();
}

} // namespace

void FolderDrive::reset() {
    s_ = State{};
    for (auto& c : s_.ch) c = Channel{};
    setStatus(73, "KERNAL64 FOLDER DRIVE");
}

void FolderDrive::setStatus(int code, const char* text, int track, int sector) {
    std::snprintf(s_.status, sizeof s_.status, "%02d,%s,%02d,%02d\r", code, text, track, sector);
    s_.statusPos = 0;
}

// ---- the bus ------------------------------------------------------------------------------------

void FolderDrive::startAtn() {
    // Everybody becomes a listener: release CLK, pull DATA ("present") within 1 ms.
    // The computer pulls ATN a moment before it takes CLK: wait for CLK to be held first, or its
    // release ("ready to send") would be mistaken for one before it was ever taken.
    s_.mode = AtnRx;
    s_.rx = RxWaitClk;
    s_.pullClk = 0;
    s_.pullData = 1;
    s_.eoi = 0;
}

void FolderDrive::tick(bool atnLow, bool clkLow, bool dataLow) {
    if (atnLow && !s_.prevAtn) startAtn();
    else if (!atnLow && s_.prevAtn) atnEnded();
    s_.prevAtn = atnLow ? 1 : 0;

    switch (s_.mode) {
    case Idle:
    case NoData:
        s_.pullClk = s_.pullData = 0;
        break;
    case AtnIgnore:
        // The byte that addressed another device is acknowledged (DATA held a moment, as the
        // frame handshake asks), then the lines are let go until ATN ends.
        s_.pullClk = 0;
        s_.pullData = ++s_.timer < kIgnoreAck ? 1 : 0;
        break;
    case AtnRx:
    case ListenRx:
        switch (s_.rx) {
        case RxWaitClk:      // the talker has not taken CLK yet
            if (clkLow) s_.rx = RxWaitTalker;
            break;
        case RxWaitTalker:   // holding DATA: busy, or acknowledging the last byte
            if (!clkLow) {   // the talker is ready to send: we are ready for data
                s_.pullData = 0;
                s_.rx = RxReady;
                s_.timer = 0;
            }
            break;
        case RxReady:
            if (clkLow) {
                s_.rx = RxBits;
                s_.bits = 0;
                s_.byte = 0;
            } else if (!s_.eoi && ++s_.timer >= kEoiTimeout) {   // the talker signals EOI: acknowledge it
                s_.eoi = 1;
                s_.pullData = 1;
                s_.rx = RxEoiAck;
                s_.timer = 0;
            }
            break;
        case RxEoiAck:
            if (++s_.timer >= kEoiAck) {
                s_.pullData = 0;
                s_.rx = RxReady;
            }
            break;
        case RxBits:
            if (!clkLow && s_.prevClk && s_.bits < 8) {          // CLK released: the bit is valid
                if (!dataLow) s_.byte = static_cast<uint8_t>(s_.byte | (1u << s_.bits));
                ++s_.bits;
            } else if (clkLow && !s_.prevClk && s_.bits == 8) {  // the frame ends: acknowledge it
                s_.pullData = 1;
                s_.rx = RxWaitTalker;
                const bool eoi = s_.eoi != 0;
                s_.eoi = 0;
                byteReceived(s_.byte, eoi);
            }
            break;
        }
        break;
    case Turnaround:
        // The computer pulls DATA and releases CLK; we let go of DATA and take CLK: we talk.
        if (!clkLow) {
            s_.pullData = 0;
            s_.pullClk = 1;
            s_.mode = Talk;
            s_.tx = TxPrepare;
            s_.timer = 0;
        }
        break;
    case Talk:
        switch (s_.tx) {
        case TxPrepare:
            s_.pullClk = 1;
            s_.pullData = 0;
            if (++s_.timer >= kByteGap) {
                bool last = false;
                uint8_t b = 0;
                if (!nextTalkByte(b, last)) {
                    // Nothing to send: let go; the computer times out (its "file not found").
                    s_.mode = NoData;
                    break;
                }
                s_.txByte = b;
                s_.txLast = last ? 1 : 0;
                s_.pullClk = 0;   // ready to send
                s_.tx = TxWaitListener;
            }
            break;
        case TxWaitListener:
            if (!dataLow) {   // the listener is ready for data
                s_.tx = s_.txLast ? TxEoiWait : TxBitSet;
                s_.bits = 0;
                s_.timer = 0;
            }
            break;
        case TxEoiWait:   // the last byte: keep CLK released until the listener acknowledges EOI
            if (dataLow) s_.tx = TxEoiRelease;
            break;
        case TxEoiRelease:
            if (!dataLow) { s_.tx = TxBitSet; s_.bits = 0; s_.timer = 0; }
            break;
        case TxBitSet:    // CLK pulled, the bit on DATA (released = 1)
            s_.pullClk = 1;
            s_.pullData = ((s_.txByte >> s_.bits) & 1) ? 0 : 1;
            if (++s_.timer >= kBitSetup) {
                s_.pullClk = 0;   // the bit is valid
                s_.tx = TxBitValid;
                s_.timer = 0;
            }
            break;
        case TxBitValid:
            if (++s_.timer >= kBitValid) {
                s_.pullClk = 1;
                s_.pullData = 0;
                s_.timer = 0;
                s_.tx = ++s_.bits == 8 ? TxWaitAck : TxBitSet;
            }
            break;
        case TxWaitAck:
            if (dataLow) {
                s_.timer = 0;
                if (s_.txLast) { s_.mode = NoData; break; }   // after EOI both let go
                s_.tx = TxPrepare;
            } else if (++s_.timer >= kAckTimeout) {
                s_.mode = NoData;
            }
            break;
        }
        break;
    }
    s_.prevClk = clkLow ? 1 : 0;
}

void FolderDrive::atnEnded() {
    if (s_.mode == AtnIgnore) { s_.mode = Idle; s_.pullClk = s_.pullData = 0; return; }
    if (s_.mode != AtnRx) return;
    if (s_.listening) {
        s_.mode = ListenRx;   // still holding DATA: the computer talks next, CLK held
        s_.rx = RxWaitClk;
        s_.pullData = 1;
    } else if (s_.talking && s_.secondary == 0x60) {
        s_.mode = Turnaround;
        s_.pullData = 1;
        s_.pullClk = 0;
    } else {
        s_.mode = Idle;
        s_.pullClk = s_.pullData = 0;
    }
}

// ---- the DOS ------------------------------------------------------------------------------------

void FolderDrive::byteReceived(uint8_t b, bool eoi) {
    static const bool trace = std::getenv("K64_FOLDER_TRACE") != nullptr;   // instrument: every byte received
    if (trace) std::fprintf(stderr, "folder: %s %02x%s\n", s_.mode == AtnRx ? "ATN " : "data", b, eoi ? " EOI" : "");
    if (s_.mode == AtnRx) {
        if (b == 0x3F) {                 // UNLISTEN
            if (s_.listening) {
                if (s_.secondary == 0xF0) openChannel();
                else if (s_.secondary == 0x60 && s_.channel == 15 && s_.cmdLen)
                    command(std::string(s_.cmdBuf, s_.cmdLen)), s_.cmdLen = 0;
            }
            s_.listening = 0;
            s_.addressed = 0;
        } else if (b == 0x5F) {          // UNTALK
            s_.talking = 0;
            s_.addressed = 0;
        } else if (b >= 0x20 && b < 0x3F) {
            s_.addressed = (b & 0x1F) == device_ ? 1 : 0;
            if (s_.addressed) { s_.listening = 1; s_.talking = 0; }
        } else if (b >= 0x40 && b < 0x5F) {
            s_.addressed = (b & 0x1F) == device_ ? 1 : 0;
            s_.talking = s_.addressed;
            if (s_.addressed) s_.listening = 0;
        }
        if (((b >= 0x20 && b < 0x3F) || (b >= 0x40 && b < 0x5F)) && !s_.addressed) {
            // Addressed to another device: off the bus until ATN ends, as a 1541 does. The
            // rest of this ATN sequence (the secondary address) is not acknowledged by us, so
            // the computer sees an absent device as absent (KERNAL $F3ED: ?DEVICE NOT PRESENT).
            s_.mode = AtnIgnore;
            s_.timer = 0;
            return;
        } else if (b >= 0x60 && s_.addressed) {   // the secondary address
            s_.channel = static_cast<uint8_t>(b & 0x0F);
            s_.secondary = static_cast<uint8_t>(b & 0xF0);
            if (s_.secondary == 0xE0) closeChannel(s_.channel);
            else if (s_.secondary == 0xF0) s_.nameLen = 0;
            else if (s_.secondary == 0x60 && s_.channel == 15 && s_.listening) s_.cmdLen = 0;
        }
        return;
    }
    // Data after ATN, to us as the listener.
    if (s_.secondary == 0xF0) {
        if (s_.nameLen < sizeof s_.nameBuf) s_.nameBuf[s_.nameLen++] = static_cast<char>(b);
    } else if (s_.channel == 15) {
        if (s_.cmdLen < sizeof s_.cmdBuf) s_.cmdBuf[s_.cmdLen++] = static_cast<char>(b);
    } else {
        Channel& c = s_.ch[s_.channel];
        if (c.open && c.write) {
            if (c.len < kSlotBytes) buf_[c.slot][c.len++] = b;
            else setStatus(72, "DISK FULL");
        }
    }
}

int FolderDrive::takeSlot() {
    for (int i = 0; i < kSlots; ++i)
        if (!s_.slotUsed[i]) { s_.slotUsed[i] = 1; return i; }
    return -1;
}

void FolderDrive::closeChannel(int ch) {
    Channel& c = s_.ch[ch];
    if (c.open && c.write && store_) {
        std::vector<uint8_t> data(buf_[c.slot].begin(), buf_[c.slot].begin() + c.len);
        if (!store_->write(c.name, c.seq != 0, data)) setStatus(26, "WRITE PROTECT ON");
    }
    if (c.open && c.slot < kSlots) s_.slotUsed[c.slot] = 0;
    c = Channel{};
}

void FolderDrive::openChannel() {
    const int ch = s_.channel;
    std::string name(s_.nameBuf, s_.nameLen);
    if (ch == 15) {
        if (!name.empty()) command(name);
        return;
    }
    closeChannel(ch);
    bool replace = false;
    if (!name.empty() && name[0] == '@') { replace = true; name.erase(0, 1); }
    if (name.size() >= 2 && name[1] == ':' && name[0] == '0') name.erase(0, 2);
    else if (!name.empty() && name[0] == ':') name.erase(0, 1);
    char type = 0, mode = 0;
    const size_t comma = name.find(',');
    if (comma != std::string::npos) {
        std::string rest = name.substr(comma + 1);
        name.erase(comma);
        if (!rest.empty()) type = rest[0];
        const size_t c2 = rest.find(',');
        if (c2 != std::string::npos && c2 + 1 < rest.size()) mode = rest[c2 + 1];
    }
    const bool write = ch == 1 || mode == 'W' || mode == 'A';
    Channel& c = s_.ch[ch];
    if (!store_) { setStatus(74, "DRIVE NOT READY"); return; }
    const int slot = takeSlot();
    if (slot < 0) { setStatus(70, "NO CHANNEL"); return; }
    c.slot = static_cast<uint8_t>(slot);

    if (!write && !name.empty() && name[0] == '$') {   // the listing, as a BASIC program
        const auto l = listing();
        c.len = static_cast<uint32_t>(std::min(l.size(), kSlotBytes));
        std::memcpy(buf_[slot].data(), l.data(), c.len);
        c.open = 1;
        setStatus(0, " OK");
        return;
    }
    if (name.empty()) { s_.slotUsed[slot] = 0; c = Channel{}; setStatus(34, "SYNTAX ERROR"); return; }

    if (write) {
        std::string host;
        for (char p : name) host += petsciiToHost(static_cast<uint8_t>(p));
        const bool seq = type == 'S' || (type == 0 && ch >= 2);
        bool exists = false;
        for (const auto& e : store_->list()) exists = exists || toPetsciiName(e.name) == toPetsciiName(host);
        if (exists && !replace) { s_.slotUsed[slot] = 0; c = Channel{}; setStatus(63, "FILE EXISTS"); return; }
        std::snprintf(c.name, sizeof c.name, "%s", host.c_str());
        c.write = 1;
        c.seq = seq ? 1 : 0;
        c.open = 1;
        setStatus(0, " OK");
        return;
    }
    for (const auto& e : store_->list()) {
        if (!matches(name, toPetsciiName(e.name))) continue;
        if ((type == 'S' && !e.seq) || (type == 'P' && e.seq)) continue;
        std::vector<uint8_t> data;
        if (!store_->read(e.name, e.seq, data)) break;
        c.len = static_cast<uint32_t>(std::min(data.size(), kSlotBytes));
        std::memcpy(buf_[slot].data(), data.data(), c.len);
        std::snprintf(c.name, sizeof c.name, "%s", e.name.c_str());
        c.seq = e.seq ? 1 : 0;
        c.open = 1;
        setStatus(0, " OK");
        return;
    }
    s_.slotUsed[slot] = 0;
    c = Channel{};
    setStatus(62, "FILE NOT FOUND");
}

void FolderDrive::command(const std::string& raw) {
    std::string cmd = raw;
    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();
    if (cmd.empty()) return;
    auto arg = [&cmd]() { const size_t c = cmd.find(':'); return c == std::string::npos ? std::string() : cmd.substr(c + 1); };
    if (!store_) { setStatus(74, "DRIVE NOT READY"); return; }
    if (cmd[0] == 'S') {   // scratch: names (patterns) separated by commas
        int n = 0;
        std::string names = arg();
        size_t p = 0;
        while (p <= names.size()) {
            const size_t e = std::min(names.find(',', p), names.size());
            const std::string pat = names.substr(p, e - p);
            if (!pat.empty())
                for (const auto& f : store_->list())
                    if (matches(pat, toPetsciiName(f.name)) && store_->remove(f.name)) ++n;
            p = e + 1;
        }
        setStatus(1, "FILES SCRATCHED", n);
    } else if (cmd[0] == 'R') {   // rename: R:NEW=OLD
        const std::string a = arg();
        const size_t eq = a.find('=');
        if (eq == std::string::npos) { setStatus(34, "SYNTAX ERROR"); return; }
        std::string to, from = a.substr(eq + 1);
        for (char c : a.substr(0, eq)) to += petsciiToHost(static_cast<uint8_t>(c));
        for (const auto& f : store_->list())
            if (toPetsciiName(f.name) == to.substr(0, 16)) { setStatus(63, "FILE EXISTS"); return; }
        for (const auto& f : store_->list())
            if (matches(from, toPetsciiName(f.name))) {
                const bool ok = store_->rename(f.name, to);
                setStatus(ok ? 0 : 26, ok ? " OK" : "WRITE PROTECT ON");
                return;
            }
        setStatus(62, "FILE NOT FOUND");
    } else if (cmd[0] == 'I' || cmd[0] == 'V') {
        setStatus(0, " OK");
    } else if (cmd == "UJ" || cmd == "U:" || cmd == "U9") {
        reset();
    } else {
        setStatus(31, "SYNTAX ERROR");
    }
}

bool FolderDrive::nextTalkByte(uint8_t& b, bool& last) {
    if (s_.channel == 15) {   // the status, ending with its CR; then "00, OK"
        const size_t n = std::strlen(s_.status);
        if (s_.statusPos >= n) return false;
        b = static_cast<uint8_t>(s_.status[s_.statusPos++]);
        last = s_.statusPos >= n;
        if (last) { setStatus(0, " OK"); s_.statusPos = 0; }
        return true;
    }
    Channel& c = s_.ch[s_.channel];
    if (!c.open || c.write || c.pos >= c.len) return false;
    b = buf_[c.slot][c.pos++];
    last = c.pos >= c.len;
    return true;
}

std::vector<uint8_t> FolderDrive::listing() {
    std::vector<uint8_t> out{0x01, 0x04};   // a BASIC program at $0401
    auto line = [&out](unsigned number, const std::string& text) {
        out.push_back(0x01);
        out.push_back(0x01);   // a link: LOAD relinks the lines
        out.push_back(static_cast<uint8_t>(number));
        out.push_back(static_cast<uint8_t>(number >> 8));
        for (char c : text) out.push_back(static_cast<uint8_t>(c));
        out.push_back(0);
    };
    std::string label = store_ ? toPetsciiName(store_->label()) : "FOLDER";
    label.resize(16, ' ');
    line(0, std::string("\x12\"") + label + "\" 01 2A");
    if (store_) {
        auto files = store_->list();
        std::sort(files.begin(), files.end(), [](const FolderStore::Entry& a, const FolderStore::Entry& b) { return a.name < b.name; });
        for (const auto& f : files) {
            const unsigned blocks = static_cast<unsigned>(std::min<size_t>((f.size + 253) / 254, 65535));
            std::string name = toPetsciiName(f.name);
            std::string text(blocks < 10 ? 3 : blocks < 100 ? 2 : 1, ' ');
            text += "\"" + name + "\"" + std::string(16 - name.size(), ' ') + (f.seq ? " SEQ" : " PRG");
            line(blocks, text);
        }
    }
    line(65535, "BLOCKS FREE.             ");
    out.push_back(0);
    out.push_back(0);
    return out;
}

// ---- save states --------------------------------------------------------------------------------

size_t FolderDrive::stateSize() { return sizeof(State) + kSlots * kSlotBytes; }

void FolderDrive::saveState(uint8_t* out) const {
    std::memcpy(out, &s_, sizeof s_);
    out += sizeof s_;
    for (const auto& b : buf_) { std::memcpy(out, b.data(), kSlotBytes); out += kSlotBytes; }
}

void FolderDrive::loadState(const uint8_t* in) {
    State s;
    std::memcpy(&s, in, sizeof s);
    in += sizeof s;
    if (s.mode > NoData || s.rx > RxWaitClk || s.tx > TxWaitAck) s = State{};   // not ours: power-on
    for (auto& c : s.ch)
        if (c.slot != 0xFF && c.slot >= kSlots) c = Channel{};
    s.status[sizeof s.status - 1] = 0;
    s_ = s;
    for (auto& b : buf_) { std::memcpy(b.data(), in, kSlotBytes); in += kSlotBytes; }
}

} // namespace k64
