// A drive whose disk is a folder of the host computer (PLAN "HARD DRIVES" (a), D28): a device on the
// serial bus that speaks the bus protocol (gitignore/refs/drive/serial-bus.txt: ATN, turnaround, EOI,
// frame handshake) and a small DOS over the folder's files (NAME.prg, NAME.seq): LOAD and SAVE
// (channels 0 and 1), the "$" listing, data files on channels 2-14, commands and status on 15.
// Not any real Commodore hardware: no fast loaders, no direct sector access.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k64 {

// The folder, as the frontend's side reaches it. Names are host names (no extension).
class FolderStore {
public:
    virtual ~FolderStore() = default;
    struct Entry { std::string name; bool seq; size_t size; };
    virtual std::vector<Entry> list() = 0;
    virtual bool read(const std::string& name, bool seq, std::vector<uint8_t>& out) = 0;
    virtual bool write(const std::string& name, bool seq, const std::vector<uint8_t>& data) = 0;
    virtual bool remove(const std::string& name) = 0;                     // every type of that name
    virtual bool rename(const std::string& from, const std::string& to) = 0;
    virtual std::string label() = 0;                                      // the listing's disk name
};

class FolderDrive {
public:
    static constexpr int kSlots = 3;               // files open at once (a real drive has few buffers too)
    static constexpr size_t kSlotBytes = 65536;    // the largest file it reads or writes

    void setStore(FolderStore* store) { store_ = store; }
    void setDevice(int device) { device_ = static_cast<uint8_t>(device); }
    int device() const { return device_; }
    void reset();

    // One machine cycle (about a microsecond). The lines as the others drive them: true = pulled low.
    void tick(bool atnLow, bool clkLow, bool dataLow);
    bool pullsClk() const { return s_.pullClk != 0; }
    bool pullsData() const { return s_.pullData != 0; }

    // Save states: the protocol, the channels and their buffers; constant size. The folder is outside
    // the machine: a state loaded does not undo what was written to it.
    static size_t stateSize();
    void saveState(uint8_t* out) const;
    void loadState(const uint8_t* in);

private:
    enum Mode : uint8_t { Idle, AtnRx, ListenRx, Turnaround, Talk, NoData };
    enum Rx : uint8_t { RxWaitTalker, RxReady, RxEoiAck, RxBits, RxWaitClk };
    enum Tx : uint8_t { TxPrepare, TxWaitListener, TxEoiWait, TxEoiRelease, TxBitSet, TxBitValid, TxWaitAck };

#pragma pack(push, 1)
    struct Channel {
        uint8_t open = 0, write = 0, seq = 0, slot = 0xFF;
        char name[24] = {};
        uint32_t pos = 0, len = 0;
    };
    struct State {
        uint8_t mode = Idle, rx = RxWaitTalker, tx = TxPrepare;
        uint8_t pullClk = 0, pullData = 0, prevAtn = 0, prevClk = 0;
        uint32_t timer = 0;
        uint8_t byte = 0, bits = 0, eoi = 0;
        uint8_t listening = 0, talking = 0, channel = 0, secondary = 0;   // secondary: 0x60 data, 0xE0 close, 0xF0 open
        uint8_t addressed = 0;   // the last LISTEN or TALK named this device: a secondary address is ours
        uint8_t txByte = 0, txLast = 0;
        uint8_t nameLen = 0;
        char nameBuf[48] = {};
        uint8_t cmdLen = 0;
        char cmdBuf[48] = {};
        char status[48] = {};
        uint8_t statusPos = 0;
        Channel ch[16];
        uint8_t slotUsed[kSlots] = {};
    };
#pragma pack(pop)

    void startAtn();
    void byteReceived(uint8_t b, bool eoi);
    void atnEnded();
    void openChannel();
    void closeChannel(int ch);
    void command(const std::string& cmd);
    void setStatus(int code, const char* text, int track = 0, int sector = 0);
    bool nextTalkByte(uint8_t& b, bool& last);
    int takeSlot();
    std::vector<uint8_t> listing();

    FolderStore* store_ = nullptr;
    uint8_t device_ = 10;
    State s_;
    std::array<std::array<uint8_t, kSlotBytes>, kSlots> buf_{};
};

} // namespace k64
