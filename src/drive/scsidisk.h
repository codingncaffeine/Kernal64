// A SCSI-2 direct-access device (a hard disk) as a target on the bus: selection, the
// information transfer phases with the REQ/ACK handshake, and the commands a CMD HD sends.
// Rules: the SCSI-2 draft rev. 10L (gitignore/refs/scsi/scsi2-r10l.pdf): phases table 8 and
// 6.1.5.1 (asynchronous transfer), status table 27, sense data table 65, INQUIRY table 45,
// READ CAPACITY tables 124-125, direct-access commands chapter 9. Notes: hw/cmdhd.md.
//
// Lines are TRUE/FALSE as the standard names them (the board inverts them). The host side
// calls setLines() whenever its lines change and ack() for each ACK it asserts.
//
// The disk's blocks live outside the machine (a host file): a save state keeps the transfer
// in progress, not the disk, as with the folder drive.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace k64 {

// 512-byte blocks in a host file; past the file's end (a sparse blank image) reads give
// zeros. The capacity is set when the image is made.
class BlockFile {
public:
    ~BlockFile() { close(); }
    bool open(const std::string& path, uint64_t blocks);   // blocks 0 = from the file's size
    void close() { if (f_) std::fclose(f_); f_ = nullptr; }
    bool isOpen() const { return f_ != nullptr; }
    uint32_t blocks() const { return blocks_; }
    bool read(uint32_t lba, uint8_t* out);
    bool write(uint32_t lba, const uint8_t* in);
private:
    std::FILE* f_ = nullptr;
    uint32_t blocks_ = 0;
};

class ScsiDisk {
public:
    enum class Phase : uint8_t { BusFree, DataOut, DataIn, Command, Status, MessageOut, MessageIn };

    explicit ScsiDisk(int id = 0) : id_(id) {}
    void attach(BlockFile* disk) { disk_ = disk; }
    void reset();   // RST, or power on

    // The initiator's lines: SEL, ATN, RST, and the data bus when it drives it.
    void setLines(bool sel, bool atn, bool rst, uint8_t data);
    void ack();     // one ACK (true then false): the byte on the bus is taken or the next is given

    bool bsy() const { return s_.phase != Phase::BusFree; }
    bool req() const { return s_.phase != Phase::BusFree && s_.req; }
    Phase phase() const { return s_.phase; }
    uint8_t data() const { return s_.out; }   // the byte the target drives (DATA IN, STATUS, MESSAGE IN)

#pragma pack(push, 1)
    struct State {
        Phase phase = Phase::BusFree;
        uint8_t req = 0;
        uint8_t out = 0;
        uint8_t cdb[12] = {};
        uint8_t cdbLen = 0, cdbPos = 0;
        uint8_t status = 0;
        uint8_t senseKey = 0, asc = 0;
        uint32_t lba = 0, count = 0;   // blocks left in a READ/WRITE
        uint16_t pos = 0, len = 0;     // the buffer's transfer
        uint8_t writing = 0;           // the DATA OUT phase of a WRITE
        uint8_t buf[512] = {};
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    void setState(const State& s) { s_ = s; }

    bool trace = false;   // K64_SCSI_TRACE: commands to stderr

private:
    void execute();
    void startData(Phase p, uint16_t len);
    void toStatus(uint8_t status);
    void checkCondition(uint8_t key, uint8_t asc);
    void nextBlockIn();

    int id_;
    BlockFile* disk_ = nullptr;
    State s_;
};

} // namespace k64
