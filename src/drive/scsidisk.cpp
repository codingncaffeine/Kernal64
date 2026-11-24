// SCSI-2 direct-access target. Rules: SCSI-2 draft rev. 10L (header).
#include "scsidisk.h"

#include <cstring>

namespace k64 {

namespace {
// 64-bit file positions: an image may be gigabytes, and `long` is 32 bits on Windows.
int seek64(std::FILE* f, uint64_t pos) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(pos), SEEK_SET);
#else
    return fseeko(f, static_cast<off_t>(pos), SEEK_SET);
#endif
}
uint64_t size64(std::FILE* f) {
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    return static_cast<uint64_t>(_ftelli64(f));
#else
    fseeko(f, 0, SEEK_END);
    return static_cast<uint64_t>(ftello(f));
#endif
}
} // namespace

// ---- BlockFile -------------------------------------------------------------------

bool BlockFile::open(const std::string& path, uint64_t blocks) {
    close();
    f_ = std::fopen(path.c_str(), "r+b");
    if (!f_) return false;
    if (!blocks) {
        blocks = size64(f_) / 512;
    }
    blocks_ = static_cast<uint32_t>(blocks > 0xFFFFFFFFu ? 0xFFFFFFFFu : blocks);
    return true;
}

bool BlockFile::read(uint32_t lba, uint8_t* out) {
    if (!f_ || lba >= blocks_) return false;
    std::memset(out, 0, 512);
    if (seek64(f_, uint64_t(lba) * 512) != 0) return false;
    (void)std::fread(out, 1, 512, f_);   // short past the end of a sparse file: zeros
    return true;
}

bool BlockFile::write(uint32_t lba, const uint8_t* in) {
    if (!f_ || lba >= blocks_) return false;
    if (seek64(f_, uint64_t(lba) * 512) != 0) return false;
    const bool ok = std::fwrite(in, 1, 512, f_) == 512;
    std::fflush(f_);
    return ok;
}

// ---- ScsiDisk --------------------------------------------------------------------

namespace {
constexpr uint8_t kGood = 0x00, kCheckCondition = 0x02;   // table 27
constexpr uint8_t kNoSense = 0, kNotReady = 2, kMediumError = 3, kIllegalRequest = 5;
// Command length by group code (7.2.1: group 0 six bytes, groups 1 and 2 ten, group 5 twelve).
uint8_t cdbLength(uint8_t op) {
    switch (op >> 5) {
    case 0: return 6;
    case 1: case 2: return 10;
    case 5: return 12;
    default: return 6;
    }
}
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
} // namespace

void ScsiDisk::reset() {
    s_ = State{};
    s_.senseKey = 6;   // UNIT ATTENTION after a reset (power on, 7.9)
    s_.asc = 0x29;
}

void ScsiDisk::setLines(bool sel, bool atn, bool rst, uint8_t data) {
    if (rst) { const uint8_t k = s_.senseKey; reset(); s_.senseKey = k ? k : 6; return; }
    (void)atn;   // MESSAGE OUT after selection with ATN: not used by the host (hw/cmdhd.md)
    // Selection (5.1.3): SEL true, BSY false, the target's ID bit on the data bus. The
    // target answers with BSY; the command phase follows once SEL is false.
    if (s_.phase == Phase::BusFree) {
        if (sel && (data & (1u << id_))) { s_.phase = Phase::Command; s_.req = 0; s_.cdbPos = 0; s_.cdbLen = 0; }
        return;
    }
    if (s_.phase == Phase::Command && !s_.req && !sel && s_.cdbPos == 0) s_.req = 1;
    s_.out = s_.phase == Phase::DataOut || s_.phase == Phase::Command ? data : s_.out;
}

void ScsiDisk::ack() {
    if (!req()) return;
    switch (s_.phase) {
    case Phase::Command:
        if (s_.cdbPos == 0) s_.cdbLen = cdbLength(s_.out);
        s_.cdb[s_.cdbPos++] = s_.out;
        if (s_.cdbPos >= s_.cdbLen) execute();
        return;
    case Phase::DataOut:
        s_.buf[s_.pos++] = s_.out;
        if (s_.pos < s_.len) return;
        if (s_.writing) {
            if (!disk_ || !disk_->write(s_.lba, s_.buf)) { checkCondition(kMediumError, 0x0C); return; }
            ++s_.lba;
            if (--s_.count) { s_.pos = 0; return; }
        }
        toStatus(kGood);
        return;
    case Phase::DataIn:
        if (++s_.pos < s_.len) { s_.out = s_.buf[s_.pos]; return; }
        if (s_.count) { nextBlockIn(); return; }
        toStatus(s_.status);
        return;
    case Phase::Status:
        s_.phase = Phase::MessageIn;
        s_.out = 0x00;   // COMMAND COMPLETE (table 10)
        return;
    case Phase::MessageIn:
        s_ = State{s_.phase, 0, 0, {}, 0, 0, 0, s_.senseKey, s_.asc};
        s_.phase = Phase::BusFree;   // the target releases BSY after COMMAND COMPLETE
        return;
    default:
        return;
    }
}

void ScsiDisk::startData(Phase p, uint16_t len) {
    s_.phase = p;
    s_.pos = 0;
    s_.len = len;
    s_.req = 1;
    if (p == Phase::DataIn) s_.out = s_.buf[0];
    if (len == 0) toStatus(s_.status);
}

void ScsiDisk::toStatus(uint8_t status) {
    s_.phase = Phase::Status;
    s_.out = status;
    s_.req = 1;
}

void ScsiDisk::checkCondition(uint8_t key, uint8_t asc) {
    s_.senseKey = key;
    s_.asc = asc;
    s_.count = 0;
    toStatus(kCheckCondition);
}

void ScsiDisk::nextBlockIn() {
    if (!disk_ || !disk_->read(s_.lba, s_.buf)) { checkCondition(kMediumError, 0x11); return; }
    ++s_.lba;
    --s_.count;
    s_.status = kGood;
    startData(Phase::DataIn, 512);
}

void ScsiDisk::execute() {
    const uint8_t* c = s_.cdb;
    const uint8_t op = c[0];
    if (trace) std::fprintf(stderr, "scsi: %02X %02X %02X %02X %02X %02X\n", c[0], c[1], c[2], c[3], c[4], c[5]);
    std::memset(s_.buf, 0, sizeof s_.buf);
    s_.status = kGood;
    s_.writing = 0;
    const bool ready = disk_ && disk_->isOpen();
    const uint8_t lun = c[1] >> 5;
    if (lun != 0 && op != 0x03 && op != 0x12) { checkCondition(kIllegalRequest, 0x25); return; }   // LUN not supported
    switch (op) {
    case 0x00:   // TEST UNIT READY (8.2.16)
        if (!ready) { checkCondition(kNotReady, 0x3A); return; }
        toStatus(kGood);
        return;
    case 0x03: {   // REQUEST SENSE (8.2.14, table 65)
        s_.buf[0] = 0x70;
        s_.buf[2] = s_.senseKey;
        s_.buf[7] = 10;
        s_.buf[12] = s_.asc;
        s_.senseKey = kNoSense;
        s_.asc = 0;
        startData(Phase::DataIn, static_cast<uint16_t>(c[4] < 18 ? c[4] : 18));
        return;
    }
    case 0x12: {   // INQUIRY (8.2.5, table 45)
        s_.buf[0] = lun ? 0x7F : 0x00;   // direct-access device; an absent LUN: qualifier 3
        s_.buf[2] = 2;                   // SCSI-2
        s_.buf[3] = 2;                   // response data format
        s_.buf[4] = 31;
        std::memcpy(s_.buf + 8, "KERNAL64", 8);
        std::memcpy(s_.buf + 16, "HARD DISK       ", 16);
        std::memcpy(s_.buf + 32, "1.0 ", 4);
        startData(Phase::DataIn, static_cast<uint16_t>(c[4] < 36 ? c[4] : 36));
        return;
    }
    case 0x25: {   // READ CAPACITY (9.2.7, table 125): the last block's address, block length
        if (!ready) { checkCondition(kNotReady, 0x3A); return; }
        const uint32_t last = disk_->blocks() - 1;
        s_.buf[0] = uint8_t(last >> 24); s_.buf[1] = uint8_t(last >> 16); s_.buf[2] = uint8_t(last >> 8); s_.buf[3] = uint8_t(last);
        s_.buf[6] = 0x02;   // 512
        startData(Phase::DataIn, 8);
        return;
    }
    case 0x08: case 0x28: case 0x0A: case 0x2A: {   // READ/WRITE (6) and (10) (9.2.5, 9.2.6, 9.2.14, 9.2.15)
        if (!ready) { checkCondition(kNotReady, 0x3A); return; }
        uint32_t lba, n;
        if (op < 0x20) { lba = (uint32_t(c[1] & 0x1F) << 16) | (c[2] << 8) | c[3]; n = c[4] ? c[4] : 256; }
        else { lba = be32(c + 2); n = (c[7] << 8) | c[8]; }
        if (n == 0) { toStatus(kGood); return; }
        if (uint64_t(lba) + n > disk_->blocks()) { checkCondition(kIllegalRequest, 0x21); return; }   // LBA out of range
        s_.lba = lba;
        s_.count = n;
        if (op == 0x08 || op == 0x28) { nextBlockIn(); return; }
        s_.writing = 1;
        startData(Phase::DataOut, 512);
        return;
    }
    case 0x04:   // FORMAT UNIT (9.2.1): nothing to lay out on an image
    case 0x1B:   // START STOP UNIT (9.2.17)
    case 0x2F:   // VERIFY (9.2.19): the image cannot have a bad block
    case 0x16: case 0x17:   // RESERVE, RELEASE (9.2.12, 9.2.11)
    case 0x1E:   // PREVENT ALLOW MEDIUM REMOVAL (9.2.4)
    case 0x35:   // SYNCHRONIZE CACHE (9.2.18)
        if (!ready) { checkCondition(kNotReady, 0x3A); return; }
        toStatus(kGood);
        return;
    case 0x1A: {   // MODE SENSE(6) (8.2.10): a header, no block descriptor, no pages
        s_.buf[0] = 3;
        startData(Phase::DataIn, static_cast<uint16_t>(c[4] < 4 ? c[4] : 4));
        return;
    }
    case 0x15:   // MODE SELECT(6) (8.2.8): the parameter list is taken and ignored
        startData(Phase::DataOut, c[4]);
        return;
    default:
        checkCondition(kIllegalRequest, 0x20);   // INVALID COMMAND OPERATION CODE
        return;
    }
}

} // namespace k64
