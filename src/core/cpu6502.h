// NMOS 6502 core, one bus access per call to tick().
//
// The same core is the C64's 6510 (whose I/O port at $00/$01 lives in the C64's
// memory map, not here) and the 1541's 6502.
//
// Bus cycles follow "Documentation for the NMOS 65xx/85xx Instruction Set"
// (64doc). Undocumented opcodes follow 64doc and the per-opcode bus traces; the
// two unstable ones use a magic constant: LXA $AB $EE; ANE $8B $EF, or $EE when the CPU
// was halted right after the opcode fetch (setAneMagic; hw/cpu.md rule 18).
//
// Interrupts: the IRQ line is a level, NMI is edge-detected. An instruction is
// followed by the interrupt sequence if, at the start of its last cycle, an NMI
// edge was pending or IRQ was asserted with the I flag clear. The I flag used is
// the one before that cycle, so CLI, SEI and PLP act one instruction late (an IRQ
// can still follow SEI; one arriving during CLI waits an instruction). (Mapped on
// the transistor-level simulation of the chip; see tools/cpudiff --scan.)
//
// RDY (lines are set before the cycle they affect): with RDY low, a cycle that
// follows a read does not happen. The CPU reads again at the address it read
// last (data discarded) and tries the cycle again next time; a cycle that follows
// a write goes ahead. So a run of writes, once begun, always completes, and no
// cycle after a read runs while RDY is low, whether it reads or writes. After an
// indexed access crosses a page, the address read again is the corrected one: the
// chip has fixed its address latch by then. (Mapped on the transistor-level
// simulation of the chip; see tools/cpudiff --scan.)
//
// Bus must provide: uint8_t read(uint16_t), void write(uint16_t, uint8_t).
#pragma once

#include <cstdint>

namespace k64 {

namespace flag {
constexpr uint8_t C = 0x01, Z = 0x02, I = 0x04, D = 0x08, B = 0x10, U = 0x20, V = 0x40, N = 0x80;
}

// State structs are packed: save states copy them byte for byte, so they must hold no
// padding (whose contents are undefined).
#pragma pack(push, 1)
struct CpuRegs {
    uint16_t pc = 0;
    uint8_t a = 0, x = 0, y = 0, s = 0xFD, p = flag::U | flag::I;
};
#pragma pack(pop)

namespace cpu_detail {

enum class Mode : uint8_t { Imp, Acc, Imm, Zp, Zpx, Zpy, Abs, Abx, Aby, Izx, Izy, Rel,
                            Brk, Jsr, Rti, Rts, Pha, Php, Pla, Plp, JmpAbs, JmpInd, Jam };

enum class Op : uint8_t {
    // reads
    LDA, LDX, LDY, EOR, AND, ORA, ADC, SBC, CMP, CPX, CPY, BIT, LAX, NOP, LAS,
    ANC, ALR, ARR, ANE, LXA, SBX,
    // writes
    STA, STX, STY, SAX, SHA, SHX, SHY, TAS,
    // read-modify-write
    ASL, LSR, ROL, ROR, INC, DEC, SLO, RLA, SRE, RRA, DCP, ISC,
    // implied
    TAX, TAY, TXA, TYA, TSX, TXS, INX, INY, DEX, DEY, CLC, SEC, CLI, SEI, CLV, CLD, SED,
    // branches
    BPL, BMI, BVC, BVS, BCC, BCS, BNE, BEQ,
    // everything whose behavior is its addressing mode (BRK, JSR, stack, jumps, JAM)
    SPECIAL,
};

enum class Kind : uint8_t { Read, Write, Modify, Other };

constexpr Kind kindOf(Op op) {
    if (op <= Op::SBX) return Kind::Read;
    if (op <= Op::TAS) return Kind::Write;
    if (op <= Op::ISC) return Kind::Modify;
    return Kind::Other;
}

struct Entry { Mode mode; Op op; };

// The opcode matrix, row by row ($x0..$xF).
#define E(m, o) Entry{Mode::m, Op::o}
inline constexpr Entry kOpcodes[256] = {
    E(Brk,SPECIAL),E(Izx,ORA),E(Jam,SPECIAL),E(Izx,SLO),E(Zp,NOP), E(Zp,ORA), E(Zp,ASL), E(Zp,SLO),
    E(Php,SPECIAL),E(Imm,ORA),E(Acc,ASL),   E(Imm,ANC),E(Abs,NOP),E(Abs,ORA),E(Abs,ASL),E(Abs,SLO),
    E(Rel,BPL),   E(Izy,ORA),E(Jam,SPECIAL),E(Izy,SLO),E(Zpx,NOP),E(Zpx,ORA),E(Zpx,ASL),E(Zpx,SLO),
    E(Imp,CLC),   E(Aby,ORA),E(Imp,NOP),    E(Aby,SLO),E(Abx,NOP),E(Abx,ORA),E(Abx,ASL),E(Abx,SLO),
    E(Jsr,SPECIAL),E(Izx,AND),E(Jam,SPECIAL),E(Izx,RLA),E(Zp,BIT), E(Zp,AND), E(Zp,ROL), E(Zp,RLA),
    E(Plp,SPECIAL),E(Imm,AND),E(Acc,ROL),   E(Imm,ANC),E(Abs,BIT),E(Abs,AND),E(Abs,ROL),E(Abs,RLA),
    E(Rel,BMI),   E(Izy,AND),E(Jam,SPECIAL),E(Izy,RLA),E(Zpx,NOP),E(Zpx,AND),E(Zpx,ROL),E(Zpx,RLA),
    E(Imp,SEC),   E(Aby,AND),E(Imp,NOP),    E(Aby,RLA),E(Abx,NOP),E(Abx,AND),E(Abx,ROL),E(Abx,RLA),
    E(Rti,SPECIAL),E(Izx,EOR),E(Jam,SPECIAL),E(Izx,SRE),E(Zp,NOP), E(Zp,EOR), E(Zp,LSR), E(Zp,SRE),
    E(Pha,SPECIAL),E(Imm,EOR),E(Acc,LSR),   E(Imm,ALR),E(JmpAbs,SPECIAL),E(Abs,EOR),E(Abs,LSR),E(Abs,SRE),
    E(Rel,BVC),   E(Izy,EOR),E(Jam,SPECIAL),E(Izy,SRE),E(Zpx,NOP),E(Zpx,EOR),E(Zpx,LSR),E(Zpx,SRE),
    E(Imp,CLI),   E(Aby,EOR),E(Imp,NOP),    E(Aby,SRE),E(Abx,NOP),E(Abx,EOR),E(Abx,LSR),E(Abx,SRE),
    E(Rts,SPECIAL),E(Izx,ADC),E(Jam,SPECIAL),E(Izx,RRA),E(Zp,NOP), E(Zp,ADC), E(Zp,ROR), E(Zp,RRA),
    E(Pla,SPECIAL),E(Imm,ADC),E(Acc,ROR),   E(Imm,ARR),E(JmpInd,SPECIAL),E(Abs,ADC),E(Abs,ROR),E(Abs,RRA),
    E(Rel,BVS),   E(Izy,ADC),E(Jam,SPECIAL),E(Izy,RRA),E(Zpx,NOP),E(Zpx,ADC),E(Zpx,ROR),E(Zpx,RRA),
    E(Imp,SEI),   E(Aby,ADC),E(Imp,NOP),    E(Aby,RRA),E(Abx,NOP),E(Abx,ADC),E(Abx,ROR),E(Abx,RRA),
    E(Imm,NOP),   E(Izx,STA),E(Imm,NOP),    E(Izx,SAX),E(Zp,STY), E(Zp,STA), E(Zp,STX), E(Zp,SAX),
    E(Imp,DEY),   E(Imm,NOP),E(Imp,TXA),    E(Imm,ANE),E(Abs,STY),E(Abs,STA),E(Abs,STX),E(Abs,SAX),
    E(Rel,BCC),   E(Izy,STA),E(Jam,SPECIAL),E(Izy,SHA),E(Zpx,STY),E(Zpx,STA),E(Zpy,STX),E(Zpy,SAX),
    E(Imp,TYA),   E(Aby,STA),E(Imp,TXS),    E(Aby,TAS),E(Abx,SHY),E(Abx,STA),E(Aby,SHX),E(Aby,SHA),
    E(Imm,LDY),   E(Izx,LDA),E(Imm,LDX),    E(Izx,LAX),E(Zp,LDY), E(Zp,LDA), E(Zp,LDX), E(Zp,LAX),
    E(Imp,TAY),   E(Imm,LDA),E(Imp,TAX),    E(Imm,LXA),E(Abs,LDY),E(Abs,LDA),E(Abs,LDX),E(Abs,LAX),
    E(Rel,BCS),   E(Izy,LDA),E(Jam,SPECIAL),E(Izy,LAX),E(Zpx,LDY),E(Zpx,LDA),E(Zpy,LDX),E(Zpy,LAX),
    E(Imp,CLV),   E(Aby,LDA),E(Imp,TSX),    E(Aby,LAS),E(Abx,LDY),E(Abx,LDA),E(Aby,LDX),E(Aby,LAX),
    E(Imm,CPY),   E(Izx,CMP),E(Imm,NOP),    E(Izx,DCP),E(Zp,CPY), E(Zp,CMP), E(Zp,DEC), E(Zp,DCP),
    E(Imp,INY),   E(Imm,CMP),E(Imp,DEX),    E(Imm,SBX),E(Abs,CPY),E(Abs,CMP),E(Abs,DEC),E(Abs,DCP),
    E(Rel,BNE),   E(Izy,CMP),E(Jam,SPECIAL),E(Izy,DCP),E(Zpx,NOP),E(Zpx,CMP),E(Zpx,DEC),E(Zpx,DCP),
    E(Imp,CLD),   E(Aby,CMP),E(Imp,NOP),    E(Aby,DCP),E(Abx,NOP),E(Abx,CMP),E(Abx,DEC),E(Abx,DCP),
    E(Imm,CPX),   E(Izx,SBC),E(Imm,NOP),    E(Izx,ISC),E(Zp,CPX), E(Zp,SBC), E(Zp,INC), E(Zp,ISC),
    E(Imp,INX),   E(Imm,SBC),E(Imp,NOP),    E(Imm,SBC),E(Abs,CPX),E(Abs,SBC),E(Abs,INC),E(Abs,ISC),
    E(Rel,BEQ),   E(Izy,SBC),E(Jam,SPECIAL),E(Izy,ISC),E(Zpx,NOP),E(Zpx,SBC),E(Zpx,INC),E(Zpx,ISC),
    E(Imp,SED),   E(Aby,SBC),E(Imp,NOP),    E(Aby,ISC),E(Abx,NOP),E(Abx,SBC),E(Abx,INC),E(Abx,ISC),
};
#undef E

// Step at which every memory-operand instruction does its data access, once its
// address is known; the read-modify-write instructions continue with the next two.
constexpr uint8_t kAccess = 20;

} // namespace cpu_detail

template <class Bus>
class Cpu6502 {
public:
#pragma pack(push, 1)
    // The instruction in progress.
    struct State {
        CpuRegs r;
        uint8_t op = 0;
        uint8_t t = 0;          // step within the instruction; 0 = opcode fetch
        uint8_t data = 0;
        uint8_t baseHi = 0;     // high byte of the unindexed address (SHA/SHX/SHY/TAS)
        uint16_t ea = 0;
        uint16_t ptr = 0;
        bool cross = false;     // indexing crossed a page
        bool interrupt = false; // the sequence being run is IRQ/NMI/reset, not BRK
        bool reset = false;
        bool takeInterrupt = false; // decided at the end of the last instruction
        bool jammed = false;
        bool haltedBeforeWrite = false; // RDY held the CPU between an indexed probe and its write
        bool haltedAfterFetch = false;  // RDY held the CPU in the cycle after the opcode fetch
    };
#pragma pack(pop)

    explicit Cpu6502(Bus& bus) : bus_(bus) {}

    // Power-on / RESET line: runs the reset sequence (reads the vector at $FFFC).
    void reset() {
        st_ = State{};
        st_.r.s = 0;
        st_.reset = true;
        st_.takeInterrupt = true;
        nmiPending_ = false;
        nmiSeen_ = nmiLine_;
    }

    void setIrq(bool asserted) { irq_ = asserted; }
    void setNmi(bool asserted) { nmiLine_ = asserted; }
    void setRdy(bool high) { rdy_ = high; }
    // ANE's magic constant varies between chips. $EF is what C64 software is known to need;
    // the per-opcode suite was generated with $EE.
    void setAneMagic(uint8_t m) { aneMagic_ = m; }

    void tick() {
        // NMI edge detector. During the two vector fetches of an interrupt sequence the latch is
        // held clear and the detector does not sample, so a pulse that starts and ends within them
        // is lost, while a line still low afterwards counts as a new edge.
        // (During an NMI's own vector fetch the detector keeps sampling: a line held low through it
        // is the NMI being served, not a new one.)
        const bool vectorFetch = cpu_detail::kOpcodes[st_.op].mode == cpu_detail::Mode::Brk && (st_.t == 5 || st_.t == 6);
        const bool nmiSequence = vectorFetch && st_.ea == 0xFFFA;
        if (vectorFetch && !nmiSequence) {
            nmiPending_ = false;
        } else if (vectorFetch) {
            nmiPending_ = false;
            nmiSeen_ = nmiLine_;
        } else {
            if (nmiLine_ && !nmiSeen_) nmiPending_ = true;
            nmiSeen_ = nmiLine_;
        }
        // Interrupts are sampled as the cycle starts; finish() uses the sample of the last cycle.
        if (!rdy_ && lastWasRead_) {
            // Halted: the last cycle's read is made again, and the CPU goes on from the byte
            // the last repeat returns (hw/cpu.md rule 14a).
            const uint8_t v = bus_.read(lastAddr_);
            if (v != lastData_ && !pre_.st.reset) again(v);
            lastData_ = v;
            if (st_.t == kAccess) st_.haltedBeforeWrite = true;
            if (st_.t == 1) st_.haltedAfterFetch = true;
            // Halted at the start of an instruction, the chip keeps sampling interrupts: one that
            // becomes pending during the halt is taken when it ends; one already decided stays
            // decided even if its line goes away.
            // PLP loads the flags as its last cycle completes: while that cycle is stretched,
            // the chip still decides with the I flag from before it (hw/cpu.md rule 14b).
            const uint8_t p = (st_.t == 0 && st_.op == 0x28) ? pre_.st.r.p : st_.r.p;
            if (st_.t == 0 && !st_.jammed && !afterQuickBranch_)
                st_.takeInterrupt = st_.takeInterrupt || nmiPending_ || (irq_ && !(p & flag::I));
            // A halted cycle repeats the previous one, and with it that cycle's interrupt sample
            // (which an in-page branch uses as its decision, rule 9).
            poll_ = nmiPending_ || (irq_ && !(p & flag::I));
            return;
        }
        afterSequence_ = false;
        afterQuickBranch_ = false;
        pollPrev_ = poll_;
        poll_ = nmiPending_ || (irq_ && !(st_.r.p & flag::I));
        pre_ = {st_, jamStep_};
        step();
    }

    // Everything the CPU holds, for save states (a plain struct of constant size).
#pragma pack(push, 1)
    struct Before {
        State st;
        uint8_t jamStep;
    };
    struct Snapshot {
        State st;
        bool irq, nmiLine, nmiSeen, nmiPending, rdy, poll, pollPrev, lastWasRead, afterSequence, afterQuickBranch;
        uint16_t lastAddr;
        uint8_t jamStep;
        uint8_t lastData;
        Before pre;
    };
#pragma pack(pop)
    Snapshot snapshot() const {
        return {st_, irq_, nmiLine_, nmiSeen_, nmiPending_, rdy_, poll_, pollPrev_, lastWasRead_, afterSequence_,
                afterQuickBranch_, lastAddr_, jamStep_, lastData_, pre_};
    }
    void restore(const Snapshot& s) {
        st_ = s.st; irq_ = s.irq; nmiLine_ = s.nmiLine; nmiSeen_ = s.nmiSeen; nmiPending_ = s.nmiPending;
        rdy_ = s.rdy; poll_ = s.poll; pollPrev_ = s.pollPrev; lastWasRead_ = s.lastWasRead;
        afterSequence_ = s.afterSequence; afterQuickBranch_ = s.afterQuickBranch; lastAddr_ = s.lastAddr;
        jamStep_ = s.jamStep; lastData_ = s.lastData; pre_ = s.pre;
    }

    CpuRegs& regs() { return st_.r; }
    const CpuRegs& regs() const { return st_.r; }
    const State& state() const { return st_; }
    State& state() { return st_; }
    bool atInstructionStart() const { return st_.t == 0; }
    bool jammed() const { return st_.jammed; }

private:
    using Mode = cpu_detail::Mode;
    using Op = cpu_detail::Op;
    using Kind = cpu_detail::Kind;
    static constexpr uint8_t kAccess = cpu_detail::kAccess;

    uint8_t rd(uint16_t a) {
        lastAddr_ = a;
        lastWasRead_ = true;
        if (!again_) lastData_ = bus_.read(a);
        return lastData_;
    }
    // Makes the last cycle again with the byte a halted repeat of its read returned. The
    // interrupt decisions taken since stay as they are.
    void again(uint8_t v) {
        const bool take = st_.takeInterrupt;
        st_ = pre_.st;
        jamStep_ = pre_.jamStep;
        afterSequence_ = false;
        afterQuickBranch_ = false;
        lastData_ = v;
        again_ = true;
        step();
        again_ = false;
        if (st_.t == 0) st_.takeInterrupt = take;
    }
    void wr(uint16_t a, uint8_t v) {
        lastAddr_ = a;
        lastWasRead_ = false;
        bus_.write(a, v);
    }
    uint16_t sp() const { return static_cast<uint16_t>(0x100 | st_.r.s); }

    void setNZ(uint8_t v) {
        st_.r.p = static_cast<uint8_t>((st_.r.p & ~(flag::N | flag::Z)) | (v & flag::N) | (v ? 0 : flag::Z));
    }
    void setFlag(uint8_t f, bool on) { st_.r.p = static_cast<uint8_t>(on ? (st_.r.p | f) : (st_.r.p & ~f)); }

    // The instruction is complete; the next cycle fetches an opcode (or starts the
    // interrupt sequence if one was pending at the end of the previous cycle).
    void finish() {
        st_.t = 0;
        st_.takeInterrupt = poll_;
    }

    // ---- ALU ---------------------------------------------------------------
    void adc(uint8_t v) {
        auto& r = st_.r;
        const unsigned c = r.p & flag::C;
        if (r.p & flag::D) {
            unsigned lo = (r.a & 0x0F) + (v & 0x0F) + c;
            if (lo > 9) lo += 6;
            unsigned hi = (r.a >> 4) + (v >> 4) + (lo > 0x0F ? 1 : 0);
            setFlag(flag::Z, ((r.a + v + c) & 0xFF) == 0);
            setFlag(flag::N, (hi & 0x08) != 0);
            setFlag(flag::V, (((hi << 4) ^ r.a) & 0x80) && !((r.a ^ v) & 0x80));
            if (hi > 9) hi += 6;
            setFlag(flag::C, hi > 0x0F);
            r.a = static_cast<uint8_t>((hi << 4) | (lo & 0x0F));
        } else {
            const unsigned sum = r.a + v + c;
            setFlag(flag::C, sum > 0xFF);
            setFlag(flag::V, (~(r.a ^ v) & (r.a ^ sum) & 0x80) != 0);
            r.a = static_cast<uint8_t>(sum);
            setNZ(r.a);
        }
    }

    void sbc(uint8_t v) {
        auto& r = st_.r;
        const unsigned borrow = (r.p & flag::C) ? 0 : 1;
        const unsigned diff = r.a - v - borrow;
        // Flags are those of the binary subtraction in both modes.
        setFlag(flag::C, diff < 0x100);
        setFlag(flag::V, ((r.a ^ v) & (r.a ^ diff) & 0x80) != 0);
        setNZ(static_cast<uint8_t>(diff));
        if (r.p & flag::D) {
            unsigned lo = (r.a & 0x0F) - (v & 0x0F) - borrow;
            unsigned hi = (r.a >> 4) - (v >> 4);
            if (lo & 0x10) { lo -= 6; --hi; }
            if (hi & 0x10) hi -= 6;
            r.a = static_cast<uint8_t>(((hi << 4) | (lo & 0x0F)) & 0xFF);
        } else {
            r.a = static_cast<uint8_t>(diff);
        }
    }

    void compare(uint8_t reg, uint8_t v) {
        const unsigned d = reg - v;
        setFlag(flag::C, reg >= v);
        setNZ(static_cast<uint8_t>(d));
    }

    void arr(uint8_t v) {
        auto& r = st_.r;
        const uint8_t t = r.a & v;
        const unsigned c = (r.p & flag::C) ? 1 : 0;
        const uint8_t res = static_cast<uint8_t>((t >> 1) | (c << 7));
        if (r.p & flag::D) {
            // 64doc's description of the decimal mode, step by step.
            setFlag(flag::N, c != 0);
            setFlag(flag::Z, res == 0);
            setFlag(flag::V, ((t ^ res) & 0x40) != 0);
            uint8_t a = res;
            const unsigned lo = t & 0x0F, hi = t >> 4;
            if (lo + (lo & 1) > 5) a = static_cast<uint8_t>((a & 0xF0) | ((a + 6) & 0x0F));
            const bool carry = hi + (hi & 1) > 5;
            if (carry) a = static_cast<uint8_t>(a + 0x60);
            setFlag(flag::C, carry);
            r.a = a;
        } else {
            r.a = res;
            setNZ(res);
            setFlag(flag::C, (res & 0x40) != 0);
            setFlag(flag::V, (((res >> 6) ^ (res >> 5)) & 1) != 0);
        }
    }

    // A read instruction's operation on its operand.
    void execRead(Op op, uint8_t v) {
        auto& r = st_.r;
        switch (op) {
        case Op::LDA: r.a = v; setNZ(v); break;
        case Op::LDX: r.x = v; setNZ(v); break;
        case Op::LDY: r.y = v; setNZ(v); break;
        case Op::EOR: r.a ^= v; setNZ(r.a); break;
        case Op::AND: r.a &= v; setNZ(r.a); break;
        case Op::ORA: r.a |= v; setNZ(r.a); break;
        case Op::ADC: adc(v); break;
        case Op::SBC: sbc(v); break;
        case Op::CMP: compare(r.a, v); break;
        case Op::CPX: compare(r.x, v); break;
        case Op::CPY: compare(r.y, v); break;
        case Op::BIT:
            setFlag(flag::Z, (r.a & v) == 0);
            r.p = static_cast<uint8_t>((r.p & ~(flag::N | flag::V)) | (v & (flag::N | flag::V)));
            break;
        case Op::LAX: r.a = r.x = v; setNZ(v); break;
        case Op::NOP: break;
        case Op::LAS: r.a = r.x = r.s = v & r.s; setNZ(r.a); break;
        case Op::ANC: r.a &= v; setNZ(r.a); setFlag(flag::C, (r.a & 0x80) != 0); break;
        case Op::ALR: r.a &= v; setFlag(flag::C, r.a & 1); r.a >>= 1; setNZ(r.a); break;
        case Op::ARR: arr(v); break;
        case Op::ANE: {
            const uint8_t magic = st_.haltedAfterFetch ? static_cast<uint8_t>(aneMagic_ & 0xEE) : aneMagic_;
            r.a = static_cast<uint8_t>((r.a | magic) & r.x & v);
            setNZ(r.a);
            break;
        }
        case Op::LXA: r.a = r.x = static_cast<uint8_t>((r.a | 0xEE) & v); setNZ(r.a); break;
        case Op::SBX: {
            const unsigned ax = r.a & r.x;
            setFlag(flag::C, ax >= v);
            r.x = static_cast<uint8_t>(ax - v);
            setNZ(r.x);
            break;
        }
        default: break;
        }
    }

    static bool isStoreHigh(Op op) { return op == Op::SHA || op == Op::SHX || op == Op::SHY || op == Op::TAS; }

    // What SHA/SHX/SHY/TAS store: a register value ANDed with the base address's high byte + 1.
    uint8_t storeHighValue(Op op) const {
        const auto& r = st_.r;
        const uint8_t reg = op == Op::SHX ? r.x : op == Op::SHY ? r.y : static_cast<uint8_t>(r.a & r.x);
        return static_cast<uint8_t>(reg & (st_.baseHi + 1));
    }

    // The value a write instruction stores (and for SHA/SHX/SHY/TAS, the address quirk).
    uint8_t storeValue(Op op) {
        auto& r = st_.r;
        auto high = [&](uint8_t v) {
            const uint8_t out = static_cast<uint8_t>(v & (st_.baseHi + 1));
            // On a page crossing the stored value also becomes the address's high byte.
            if (st_.cross) st_.ea = static_cast<uint16_t>((out << 8) | (st_.ea & 0xFF));
            // If RDY held the CPU just before this write, the AND with the high byte is not made:
            // the register value is stored as it is (at the address latched before the halt).
            return st_.haltedBeforeWrite ? v : out;
        };
        switch (op) {
        case Op::STA: return r.a;
        case Op::STX: return r.x;
        case Op::STY: return r.y;
        case Op::SAX: return r.a & r.x;
        case Op::SHA: return high(r.a & r.x);
        case Op::SHX: return high(r.x);
        case Op::SHY: return high(r.y);
        case Op::TAS: r.s = r.a & r.x; return high(r.s);
        default: return 0;
        }
    }

    uint8_t execModify(Op op, uint8_t v) {
        auto& r = st_.r;
        auto asl = [&](uint8_t x) { setFlag(flag::C, x & 0x80); return static_cast<uint8_t>(x << 1); };
        auto lsr = [&](uint8_t x) { setFlag(flag::C, x & 1); return static_cast<uint8_t>(x >> 1); };
        auto rol = [&](uint8_t x) {
            const uint8_t c = r.p & flag::C;
            setFlag(flag::C, x & 0x80);
            return static_cast<uint8_t>((x << 1) | c);
        };
        auto ror = [&](uint8_t x) {
            const uint8_t c = (r.p & flag::C) ? 0x80 : 0;
            setFlag(flag::C, x & 1);
            return static_cast<uint8_t>((x >> 1) | c);
        };
        uint8_t o = v;
        switch (op) {
        case Op::ASL: o = asl(v); setNZ(o); break;
        case Op::LSR: o = lsr(v); setNZ(o); break;
        case Op::ROL: o = rol(v); setNZ(o); break;
        case Op::ROR: o = ror(v); setNZ(o); break;
        case Op::INC: o = static_cast<uint8_t>(v + 1); setNZ(o); break;
        case Op::DEC: o = static_cast<uint8_t>(v - 1); setNZ(o); break;
        case Op::SLO: o = asl(v); r.a |= o; setNZ(r.a); break;
        case Op::RLA: o = rol(v); r.a &= o; setNZ(r.a); break;
        case Op::SRE: o = lsr(v); r.a ^= o; setNZ(r.a); break;
        case Op::RRA: o = ror(v); adc(o); break;
        case Op::DCP: o = static_cast<uint8_t>(v - 1); compare(r.a, o); break;
        case Op::ISC: o = static_cast<uint8_t>(v + 1); sbc(o); break;
        default: break;
        }
        return o;
    }

    void execImplied(Op op) {
        auto& r = st_.r;
        switch (op) {
        case Op::TAX: r.x = r.a; setNZ(r.x); break;
        case Op::TAY: r.y = r.a; setNZ(r.y); break;
        case Op::TXA: r.a = r.x; setNZ(r.a); break;
        case Op::TYA: r.a = r.y; setNZ(r.a); break;
        case Op::TSX: r.x = r.s; setNZ(r.x); break;
        case Op::TXS: r.s = r.x; break;
        case Op::INX: setNZ(++r.x); break;
        case Op::INY: setNZ(++r.y); break;
        case Op::DEX: setNZ(--r.x); break;
        case Op::DEY: setNZ(--r.y); break;
        case Op::CLC: setFlag(flag::C, false); break;
        case Op::SEC: setFlag(flag::C, true); break;
        case Op::CLI: setFlag(flag::I, false); break;
        case Op::SEI: setFlag(flag::I, true); break;
        case Op::CLV: setFlag(flag::V, false); break;
        case Op::CLD: setFlag(flag::D, false); break;
        case Op::SED: setFlag(flag::D, true); break;
        default: break;
        }
    }

    bool branchTaken(Op op) const {
        const uint8_t p = st_.r.p;
        switch (op) {
        case Op::BPL: return !(p & flag::N);
        case Op::BMI: return p & flag::N;
        case Op::BVC: return !(p & flag::V);
        case Op::BVS: return p & flag::V;
        case Op::BCC: return !(p & flag::C);
        case Op::BCS: return p & flag::C;
        case Op::BNE: return !(p & flag::Z);
        case Op::BEQ: return p & flag::Z;
        default: return false;
        }
    }

    // Indexed absolute addressing, third cycle: the low byte is indexed, the high byte not yet.
    void indexAbsolute(uint8_t lo, uint8_t hi, uint8_t index) {
        const unsigned sum = lo + index;
        st_.baseHi = hi;
        st_.cross = sum > 0xFF;
        st_.ea = static_cast<uint16_t>((hi << 8) | (sum & 0xFF));
    }

    // The cycle that reads at the possibly-wrong address of an indexed mode.
    void indexedProbe(Op op) {
        const uint8_t v = rd(st_.ea);
        if (st_.cross) {
            st_.ea = static_cast<uint16_t>(st_.ea + 0x100);
            // The address latch already holds the corrected address; for SHA/SHX/SHY/TAS it holds
            // the stored value as its high byte (their address quirk), and a halt reads there.
            lastAddr_ = isStoreHigh(op) ? static_cast<uint16_t>((storeHighValue(op) << 8) | (st_.ea & 0xFF)) : st_.ea;
        }
        if (cpu_detail::kindOf(op) == Kind::Read && !st_.cross) {
            execRead(op, v);
            finish();
        } else {
            st_.t = kAccess;
        }
    }

    void step() {
        auto& r = st_.r;
        if (st_.t == 0) {
            if (st_.jammed) { jamCycle(); return; }
            if (st_.takeInterrupt) {
                rd(r.pc); // the opcode is fetched and thrown away
                st_.op = 0x00;
                st_.interrupt = true;
            } else {
                st_.op = rd(r.pc++);
                st_.interrupt = false;
            }
            st_.t = 1;
            st_.haltedBeforeWrite = false;
            st_.haltedAfterFetch = false;
            return;
        }

        const cpu_detail::Entry e = cpu_detail::kOpcodes[st_.op];
        if (st_.t >= kAccess) { access(e.op); return; }

        switch (e.mode) {
        case Mode::Imp:
            rd(r.pc);
            execImplied(e.op);
            finish();
            return;
        case Mode::Acc:
            rd(r.pc);
            r.a = execModify(e.op, r.a);
            finish();
            return;
        case Mode::Imm:
            execRead(e.op, rd(r.pc++));
            finish();
            return;
        case Mode::Zp:
            st_.ea = rd(r.pc++);
            st_.t = kAccess;
            return;
        case Mode::Zpx:
        case Mode::Zpy:
            if (st_.t == 1) { st_.ptr = rd(r.pc++); st_.t = 2; return; }
            rd(st_.ptr);
            st_.ea = static_cast<uint8_t>(st_.ptr + (e.mode == Mode::Zpx ? r.x : r.y));
            st_.t = kAccess;
            return;
        case Mode::Abs:
            if (st_.t == 1) { st_.ea = rd(r.pc++); st_.t = 2; return; }
            st_.ea = static_cast<uint16_t>(st_.ea | (rd(r.pc++) << 8));
            st_.t = kAccess;
            return;
        case Mode::Abx:
        case Mode::Aby:
            switch (st_.t) {
            case 1: st_.data = rd(r.pc++); st_.t = 2; return;
            case 2: indexAbsolute(st_.data, rd(r.pc++), e.mode == Mode::Abx ? r.x : r.y); st_.t = 3; return;
            default: indexedProbe(e.op); return;
            }
        case Mode::Izx:
            switch (st_.t) {
            case 1: st_.ptr = rd(r.pc++); st_.t = 2; return;
            case 2: rd(st_.ptr); st_.ptr = static_cast<uint8_t>(st_.ptr + r.x); st_.t = 3; return;
            case 3: st_.ea = rd(st_.ptr); st_.t = 4; return;
            default:
                st_.ea = static_cast<uint16_t>(st_.ea | (rd(static_cast<uint8_t>(st_.ptr + 1)) << 8));
                st_.t = kAccess;
                return;
            }
        case Mode::Izy:
            switch (st_.t) {
            case 1: st_.ptr = rd(r.pc++); st_.t = 2; return;
            case 2: st_.data = rd(st_.ptr); st_.t = 3; return;
            case 3: indexAbsolute(st_.data, rd(static_cast<uint8_t>(st_.ptr + 1)), r.y); st_.t = 4; return;
            default: indexedProbe(e.op); return;
            }
        case Mode::Rel:
            switch (st_.t) {
            case 1:
                st_.data = rd(r.pc++);
                if (branchTaken(e.op)) st_.t = 2;
                else finish();
                return;
            case 2: {
                rd(r.pc);
                const uint16_t target = static_cast<uint16_t>(r.pc + static_cast<int8_t>(st_.data));
                st_.ea = target;
                r.pc = static_cast<uint16_t>((r.pc & 0xFF00) | (target & 0xFF));
                if (target == r.pc) {
                    // Taken without crossing a page: the interrupt check is the one made at the
                    // start of the previous (operand) cycle, not this one.
                    st_.t = 0;
                    st_.takeInterrupt = pollPrev_;
                    afterQuickBranch_ = true; // a halt right after does not re-sample either
                    return;
                }
                st_.t = 3;
                return;
            }
            default:
                rd(r.pc);
                // Crossing forward, the address latch is corrected in this cycle (as with indexing);
                // crossing backward it is not, and a halt right after reads the uncorrected address.
                if (!(st_.data & 0x80)) lastAddr_ = st_.ea; // forward = positive offset (wraps at $FFFF)
                r.pc = st_.ea;
                finish();
                return;
            }
        case Mode::Brk:
            interruptSequence();
            return;
        case Mode::Jsr:
            switch (st_.t) {
            case 1: st_.data = rd(r.pc++); st_.t = 2; return;
            case 2: rd(sp()); st_.t = 3; return;
            case 3: wr(sp(), static_cast<uint8_t>(r.pc >> 8)); --r.s; st_.t = 4; return;
            case 4: wr(sp(), static_cast<uint8_t>(r.pc)); --r.s; st_.t = 5; return;
            default:
                r.pc = static_cast<uint16_t>(st_.data | (rd(r.pc) << 8));
                finish();
                return;
            }
        case Mode::Rts:
            switch (st_.t) {
            case 1: rd(r.pc); st_.t = 2; return;
            case 2: rd(sp()); ++r.s; st_.t = 3; return;
            case 3: st_.data = rd(sp()); ++r.s; st_.t = 4; return;
            case 4: r.pc = static_cast<uint16_t>(st_.data | (rd(sp()) << 8)); st_.t = 5; return;
            default: rd(r.pc++); finish(); return;
            }
        case Mode::Rti:
            switch (st_.t) {
            case 1: rd(r.pc); st_.t = 2; return;
            case 2: rd(sp()); ++r.s; st_.t = 3; return;
            case 3: r.p = static_cast<uint8_t>((rd(sp()) & ~flag::B) | flag::U); ++r.s; st_.t = 4; return;
            case 4: st_.data = rd(sp()); ++r.s; st_.t = 5; return;
            default:
                r.pc = static_cast<uint16_t>(st_.data | (rd(sp()) << 8));
                finish();
                return;
            }
        case Mode::Pha:
        case Mode::Php:
            if (st_.t == 1) { rd(r.pc); st_.t = 2; return; }
            wr(sp(), e.mode == Mode::Pha ? r.a : static_cast<uint8_t>(r.p | flag::B | flag::U));
            --r.s;
            finish();
            return;
        case Mode::Pla:
        case Mode::Plp:
            switch (st_.t) {
            case 1: rd(r.pc); st_.t = 2; return;
            case 2: rd(sp()); ++r.s; st_.t = 3; return;
            default: {
                const uint8_t v = rd(sp());
                if (e.mode == Mode::Pla) { r.a = v; setNZ(v); }
                else r.p = static_cast<uint8_t>((v & ~flag::B) | flag::U);
                finish();
                return;
            }
            }
        case Mode::JmpAbs:
            if (st_.t == 1) { st_.data = rd(r.pc++); st_.t = 2; return; }
            r.pc = static_cast<uint16_t>(st_.data | (rd(r.pc) << 8));
            finish();
            return;
        case Mode::JmpInd:
            switch (st_.t) {
            case 1: st_.ptr = rd(r.pc++); st_.t = 2; return;
            case 2: st_.ptr = static_cast<uint16_t>(st_.ptr | (rd(r.pc++) << 8)); st_.t = 3; return;
            case 3: st_.data = rd(st_.ptr); st_.t = 4; return;
            default:
                // The pointer's high byte is not carried into: JMP ($xxFF) reads $xx00.
                r.pc = static_cast<uint16_t>(st_.data |
                    (rd(static_cast<uint16_t>((st_.ptr & 0xFF00) | ((st_.ptr + 1) & 0xFF))) << 8));
                finish();
                return;
            }
        case Mode::Jam:
            if (st_.t == 1) { rd(r.pc); jamStep_ = 0; st_.jammed = true; st_.t = 0; return; }
            return;
        }
    }

    // The data access of a memory-operand instruction, and the two write cycles of
    // read-modify-write.
    void access(Op op) {
        switch (cpu_detail::kindOf(op)) {
        case Kind::Read:
            execRead(op, rd(st_.ea));
            finish();
            return;
        case Kind::Write: {
            const uint8_t v = storeValue(op);
            wr(st_.ea, v);
            finish();
            return;
        }
        case Kind::Modify:
            switch (st_.t - kAccess) {
            case 0: st_.data = rd(st_.ea); st_.t = kAccess + 1; return;
            case 1: wr(st_.ea, st_.data); st_.data = execModify(op, st_.data); st_.t = kAccess + 2; return;
            default: wr(st_.ea, st_.data); finish(); return;
            }
        case Kind::Other:
            finish();
            return;
        }
    }

    // BRK, IRQ, NMI and reset share one sequence; reset reads where the others write.
    void interruptSequence() {
        auto& r = st_.r;
        const bool hw = st_.interrupt;
        switch (st_.t) {
        case 1:
            rd(r.pc);
            if (!hw) ++r.pc; // BRK skips its padding byte
            st_.t = 2;
            return;
        case 2: push(static_cast<uint8_t>(r.pc >> 8)); st_.t = 3; return;
        case 3: push(static_cast<uint8_t>(r.pc)); st_.t = 4; return;
        case 4: {
            // The B bit is pushed set for BRK, even when an NMI takes the sequence over.
            const uint8_t pushed = static_cast<uint8_t>((r.p | flag::U | (hw ? 0 : flag::B)) & (hw ? ~flag::B : 0xFF));
            // The vector is chosen as this cycle starts: an NMI pending by now takes over an IRQ
            // or BRK; one arriving later stays pending for after the handler's first instruction.
            if (st_.reset) st_.ea = 0xFFFC;
            else if (nmiPending_) { st_.ea = 0xFFFA; nmiPending_ = false; }
            else st_.ea = 0xFFFE;
            push(pushed);
            st_.t = 5;
            return;
        }
        case 5:
            st_.data = rd(st_.ea);
            r.p |= flag::I;
            st_.t = 6;
            return;
        default:
            r.pc = static_cast<uint16_t>(st_.data | (rd(static_cast<uint16_t>(st_.ea + 1)) << 8));
            st_.reset = false;
            st_.interrupt = false;
            // The handler's first instruction always runs before another interrupt is taken.
            st_.t = 0;
            st_.takeInterrupt = false;
            afterSequence_ = true;
            return;
        }
    }

    void push(uint8_t v) {
        if (st_.reset) rd(sp());
        else wr(sp(), v);
        --st_.r.s;
    }

    // A jammed CPU keeps reading; the address pattern is the one the bus traces record.
    void jamCycle() {
        static constexpr uint16_t kPattern[] = {0xFFFF, 0xFFFE, 0xFFFE};
        rd(jamStep_ < 3 ? kPattern[jamStep_] : 0xFFFF);
        if (jamStep_ < 3) ++jamStep_;
    }

    Bus& bus_;
    State st_;
    uint8_t aneMagic_ = 0xEF;
    bool irq_ = false, nmiLine_ = false, nmiSeen_ = false, nmiPending_ = false, rdy_ = true;
    bool poll_ = false, pollPrev_ = false, lastWasRead_ = true;
    bool afterSequence_ = false;    // the last cycle ended an interrupt sequence
    bool afterQuickBranch_ = false; // the last cycle ended a taken branch within its page
    uint16_t lastAddr_ = 0;
    uint8_t jamStep_ = 0;
    uint8_t lastData_ = 0;          // the byte the last read cycle returned
    Before pre_{};                  // the instruction state before the last cycle
    bool again_ = false;            // step() is repeating a cycle: its read takes lastData_
};

} // namespace k64
