// The whole machine, advanced one system cycle at a time.
//
// Per cycle (VIC-II article §2.4.3): in the first clock phase the VIC-II makes its
// access; in the second, the 6510 makes its access (when the VIC-II has not taken
// the bus). Every access goes through the PLA (pla.h).
#pragma once

#include "cpu6502.h"
#include "roms.h"
#include "timing.h"
#include "vic2.h"
#include "cia6526.h"
#include "sid.h"
#include "audio.h"
#include "cartridge.h"
#include "prg.h"
#include "c1541.h"
#include "folderdrive.h"
#include "cmdhd.h"
#include "datasette.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k64 {

class C64 final : private VicHost, private CiaHost {
public:
    explicit C64(VicModel model = VicModel::Mos6569);

    // The system ROMs; the machine does not run without BASIC, KERNAL and characters.
    void setRoms(const RomSet& roms);
    bool hasSystemRoms() const { return romsOk_; }

    void reset();       // power on
    void setModel(VicModel model);
    // Both CIAs as the original 6526 or the later 8521. Takes effect at once (no reset).
    void setCiaModel(CiaModel m) { cia1_.setModel(m); cia2_.setModel(m); }
    CiaModel ciaModel() const { return cia1_.model(); }
    // The SID as the 6581 or the 8580. Takes effect at once (no reset).
    void setSidModel(SidModel m) { sid_.setModel(m); }
    SidModel sidModel() const { return sid_.model(); }
    void setSidNoisePulseDrains(bool drains) { sid_.setNoisePulseDrains(drains); }
    const Sid& sid() const { return sid_; }
    const Timing& timing() const { return *timing_; }

    // The expansion port. A cartridge goes in and out with the machine off: reset() after.
    // False, with the reason in `error`, when the cartridge's hardware is not built.
    bool attachCartridge(const CartImage& image, std::string& error) { return cart_.attach(image, error); }
    void detachCartridge() { cart_.detach(); }
    const Cartridge& cartridge() const { return cart_; }
    // The button on a freezer cartridge; without such a cartridge it does nothing.
    void setFreezeButton(bool down) { cart_.setFreezeButton(down); }

    // Drive 8 on the serial bus: a 1541, there when its ROMs are and it is wanted (default
    // yes: the reference emulator's default machine has one). Switching it on while the
    // machine runs is switching the drive on: it starts from its power-on state.
    void setDriveWanted(bool on) {
        if (on && !driveWanted_) { drive_.setBus(false, false, false); drive_.reset(); }
        driveWanted_ = on;
    }
    bool driveAttached() const { return driveWanted_ && drive_.hasRoms(); }
    const C1541& drive() const { return drive_; }
    // The folder drive on the serial bus (a host folder as a disk; PLAN "HARD DRIVES" (a)): on with a store,
    // at the given device number. Off (null store): it is not on the bus.
    void setFolderDrive(FolderStore* store, int device) {
        folder_.setStore(store);
        folder_.setDevice(device);
        folderOn_ = store != nullptr;
    }
    bool folderDriveOn() const { return folderOn_; }
    // A CMD HD on the serial bus (PLAN Phase 11b): on when its boot ROM is set and it is
    // switched on; its disk is a host file. Switching it on starts it from power on.
    // (Not yet in save states: Phase 11b step 4.)
    CmdHd& hardDrive() { return hd_; }
    void setHardDriveOn(bool on) {
        if (on && !hdOn_) { hd_.setBus(false, false, false); hd_.reset(); hdPhase_ = 0; }
        hdOn_ = on && hd_.hasRom();
    }
    bool hardDriveOn() const { return hdOn_; }
    // The datasette on the cassette port.
    // The tape moves past the head: a button down and the motor running (what fast loading watches).
    bool tapeMoving() const {
        return tape_.hasTape() && tape_.button() != Datasette::Button::None && !((s_.portDdr & 0x20) && (s_.portData & 0x20));
    }
    // The KERNAL's pause after "FOUND name" on a tape load (it waits for the C= key or about 8 seconds, the
    // tape stopped): the loop at $E4E0-$E4EB of KERNAL 901227-03, a button of the datasette down.
    bool kernalTapePause() const {
        const uint16_t pc = cpu_.regs().pc;
        return tape_.hasTape() && tape_.button() != Datasette::Button::None && pc >= 0xE4E0 && pc <= 0xE4EB;
    }
    Datasette& datasette() { return tape_; }
    const Datasette& datasette() const { return tape_; }
    // The disk in drive 8 (it can change while the machine runs, as a disk is swapped). The
    // drive writes into it.
    void insertDisk(std::shared_ptr<GcrDisk> disk) { drive_.insertDisk(std::move(disk)); }
    void ejectDisk() { drive_.ejectDisk(); }

    // A program to put into memory and start with RUN once the KERNAL waits for a key
    // after this and every later reset() (an empty one: none). The wait can be made
    // `delayCycles` longer, to move the start against the frame.
    void setAutostart(const ProgramFile& program, uint64_t delayCycles = 0);
    // Text typed after this and every later reset(), as the KERNAL asks for keys: put into its
    // keyboard buffer ten keys at a time whenever it waits with the buffer empty ("\r" =
    // RETURN). A disk starts with LOAD"*",8,1 and RUN, as on the real machine. Empty: none.
    void setAutotype(const std::string& text) { autotype_ = text; armAutotype(); }

    // Audio: produced once a sample rate is set (the test tools run without it). Mono,
    // about -1 .. 1; whatever a frame produced is read after it.
    void setAudioRate(double sampleRate);
    size_t audioAvailable() const { return resampler_.available(); }
    size_t readAudio(float* dst, size_t max) { return resampler_.read(dst, max); }

    void runFrame();    // one video frame of cycles, ending where vertical blanking starts
    void tick();        // one system cycle

    // The raster as emitted: one palette index per pixel, kMaxRasterWidth wide, row = raster
    // line, column = pixel counted from the start of cycle 1 (Vic2::pixels).
    const uint8_t* raster() const { return vic_.pixels(); }
    const Vic2& vic() const { return vic_; }
    // The visible picture as XRGB8888, rows from the first visible line (in raster order
    // across the frame boundary), timing().visibleWidth x timing().visibleLines.
    void visibleFrame(uint32_t* out) const;

    // Input, applied by the frontend between frames. Keyboard matrix position: column =
    // CIA 1 port A bit (0-7), row = port B bit (0-7), as in the Programmer's Reference Guide.
    void setKey(int column, int row, bool down);
    void releaseAllKeys();
    // Joystick lines, active high: bit 0 up, 1 down, 2 left, 3 right, 4 fire.
    void setJoystick(int port, uint8_t lines);   // port 1 or 2
    void setRestore(bool down);                  // RESTORE goes straight to the NMI line
    // What sits on a control port's two paddle lines, as the value (0-255) the SID's
    // register shows when that port alone is switched through; -1 = nothing on the line.
    void setPaddles(int port, int x, int y);     // port 1 or 2
    // A light pen on control port 1 looking at the picture at raster `line`, the beam's
    // `cycle` in it (1-based): it pulls LP low in that cycle of every frame (hw/input.md L).
    // line < 0: it looks at nothing.
    void setLightPen(int line, int cycle);

    // Test harness only: the reference emulator's debug cartridge. A write to $D7FF ends a
    // test program with that value as its result (0 = passed). Off unless enabled.
    void enableDebugCart(bool on) { debugCart_ = on; debugExit_ = -1; }
    int debugCartResult() const { return debugExit_; }

    // Test harness only: memory writes from outside the CPU (program injection).
    void pokeRam(uint16_t addr, uint8_t v) { ram_[addr] = v; }

    uint64_t cycle() const { return s_.cycle; }
    uint64_t frame() const { return s_.frame; }

    // Read-only views for tests and tools.
    const uint8_t* ram() const { return ram_.data(); }
    uint8_t* ramData() { return ram_.data(); }   // the 64 KB of RAM, for the frontend (cheats, achievements)
    const CpuRegs& cpuRegs() const { return cpu_.regs(); }
    bool cpuAtInstructionStart() const { return cpu_.atInstructionStart(); }   // the next access fetches an opcode
    uint8_t peekCpu(uint16_t addr) const;  // what the CPU would read, without side effects

    // Machine state as a flat byte image of constant size for the model.
    size_t stateSize() const;
    void saveState(uint8_t* out) const;
    bool loadState(const uint8_t* in, size_t size);
    bool stateFits(const uint8_t* in, size_t size) const;   // loadState would take it

private:
    struct CpuBus {
        C64* m;
        uint8_t read(uint16_t a) { return m->cpuRead(a); }
        void write(uint16_t a, uint8_t v) { m->cpuWrite(a, v); }
    };

    uint8_t cpuRead(uint16_t a);
    uint8_t cpuReadBus(uint16_t a);
    void cpuWrite(uint16_t a, uint8_t v);
    void cpuWriteBus(uint16_t a, uint8_t v);
    uint8_t ioRead(uint16_t a);
    void ioWrite(uint16_t a, uint8_t v);
    uint8_t vicFetch(uint16_t vaddr14, bool cpuHalf) const;  // a VIC-II access in its 16 KB view
    uint8_t vicRead(uint16_t a, bool cpuHalf) override { return vicFetch(a, cpuHalf); }
    uint8_t ciaPortA(int cia, uint8_t driven) override;
    uint8_t ciaPortB(int cia, uint8_t driven) override;
    void keyboardLines(uint8_t paDriven, uint8_t pbDriven, uint8_t& pa, uint8_t& pb) const;
    uint8_t colorRead(uint16_t a) override { return colorRam_[a & 0x3FF]; }
    bool vicIsCharRom(uint16_t vaddr14) const override;
    void updateLightPen();   // CIA 1 port B bit 4 is the VIC-II's light pen input
    void updatePots();       // the control ports' paddle lines through the switch to the SID
    uint16_t plaInputsVic(uint16_t vaddr14, bool cpuHalf = false) const;
    void injectProgram();

    uint16_t plaInputsCpu(uint16_t a, bool read) const;

    // The CPU is running the KERNAL ROM: its program counter is in $E000-$FFFF and the PLA

    // selects the KERNAL there.

    bool cpuInKernal() const;
    uint8_t portPins() const;    // the 6510 port lines as the PLA sees them
    uint8_t portRead() const;    // reading $01
    unsigned vicBank() const;    // 0..3, from CIA 2 port A (inverted)

    // ---- state (everything here is saved; packed so it holds no padding) -------
#pragma pack(push, 1)
    struct State {
        uint64_t cycle = 0, frame = 0;
        uint8_t portDdr = 0, portData = 0;
        uint8_t portFloat = 0;           // bits 3, 6, 7 as last driven (they float when inputs)
        uint8_t vicLastByte = 0;         // what the VIC-II read in the last first phase (open bus)
        uint8_t cpuData = 0;             // the CPU's data bus in the last second phase
        uint8_t aecLow = 0;              // the VIC-II holds the second phase of this cycle
        uint8_t baLowSeen = 0;           // BA in the last cycle: what RDY tells the CPU now
        uint8_t keys[8] = {};             // pressed keys: bit = row, per column
        uint8_t joy[2] = {};              // control ports 1 and 2, active high
        uint8_t restore = 0;
        int16_t pot[2][2] = {{-1, -1}, {-1, -1}};   // paddle lines [port][x, y]; -1 = open
        uint8_t potAny = 0;               // something is on a paddle line
        uint8_t autostartPending = 0;     // the program is still to be put into memory
        uint64_t autostartReadyAt = 0;    // the cycle the KERNAL first waited for a key in
        uint8_t cia1IrqSeen = 0, cia2IrqSeen = 0, vicIrqSeen = 0;   // the interrupt lines as the CPU sees them
        uint32_t todCount = 0;            // mains phase: += mainsHz per cycle, a tick per clock rate
        uint32_t drivePhase = 0;
        uint16_t autotypePos = 0;
        // The text being typed, in the state: a state loaded later types what was waiting when it was
        // saved, not what the content of the new session would start with.
        uint8_t autotypeLen = 0;
        char autotype[48] = {};
        int16_t penLine = -1, penCycle = 0;   // where a light pen points (raster line, cycle); -1 = nowhere
        uint8_t penLow = 0;               // the pen pulls LP low in this cycle (hw/input.md L3)
        uint8_t busOut = 0x3F;            // CIA 2's bus outputs before its last access (S5)         // keys of the autotype text already typed          // the drive's clock against ours: += driveNum_ per cycle, a drive cycle per driveDen_
    };
#pragma pack(pop)

    const Timing* timing_;
    State s_;
    Cartridge cart_;
    ProgramFile autostart_;      // settings, like the ROMs: not part of the state
    uint64_t autostartDelay_ = 0;
    std::string autotype_;       // the setting: copied into the state at reset (armAutotype)
    void armAutotype() {
        s_.autotypeLen = static_cast<uint8_t>(std::min(autotype_.size(), sizeof s_.autotype));
        std::memcpy(s_.autotype, autotype_.data(), s_.autotypeLen);
        s_.autotypePos = 0;
    }
    std::array<uint8_t, 65536> ram_{};
    std::array<uint8_t, 1024> colorRam_{};
    std::array<uint8_t, 8192> basic_{}, kernal_{};
    std::array<uint8_t, 4096> chargen_{};
    bool romsOk_ = false;
    bool debugCart_ = false;
    int debugExit_ = -1;
    C1541 drive_;
    FolderDrive folder_;
    bool folderOn_ = false;
    CmdHd hd_;
    bool hdOn_ = false;
    uint64_t hdPhase_ = 0;   // the HD's 2 MHz clock against ours, as drivePhase
    Datasette tape_;
    bool driveWanted_ = true;
    uint32_t driveNum_ = 1, driveDen_ = 1;   // drive cycles per machine cycle, exactly (setModel)
    CpuBus bus_{this};
    Cpu6502<CpuBus> cpu_{bus_};
    Vic2 vic_{*this, kTimingPal};
    Cia6526 cia1_{*this, 0}, cia2_{*this, 1};
    Sid sid_;
    BoardFilter boardFilter_;
    Resampler resampler_;   // not part of a save state: it carries on across a load
    double audioRate_ = 0.0;
};

} // namespace k64
