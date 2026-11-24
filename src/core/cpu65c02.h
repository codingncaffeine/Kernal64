// CMOS 65C02 core (Rockwell R65C02 or WDC W65C02S), one bus access per call to tick().
//
// The CMD HD's processor. Behavior from the W65C02S data sheet (gitignore/refs/65c02/
// w65c02s.pdf: table 5-1 instruction set, 5-2 opcode matrix, 7-1 "operational enhancements"
// over the NMOS 6502). The data sheet gives cycle counts, not the address of each internal
// cycle; those follow the per-opcode bus traces (SingleStepTests rockwell65c02/v1 and
// wdc65c02/v1). Notes: local-notes/hw/cpu65c02.md.
//
// Differences from the NMOS core (cpu6502.h) that matter here: every undefined opcode is a
// NOP of a fixed length and duration; read-modify-write reads twice and writes once; the
// extra cycle of an index crossing a page reads the instruction's last byte again; JMP
// (ind) carries into the pointer's high byte (one more cycle); decimal ADC/SBC set N, V and
// Z from the result (one more cycle); BRK and interrupts clear D. The WDC chip adds WAI and
// STP; on the Rockwell chip their opcodes are NOPs.
//
// Interrupts: IRQ is a level, NMI an edge; the decision is the one sampled at the start of
// an instruction's last cycle, as on the NMOS chip. RDY and SO are not wired on the board
// this core serves (no input for them).
//
// Bus must provide: uint8_t read(uint16_t), void write(uint16_t, uint8_t).
#pragma once

#include <cstdint>

#include "cpu6502.h"   // CpuRegs, flag::

namespace k64 {

namespace c02 {

enum class Mode : uint8_t {
    Imp, Acc, Imm, Zp, Zpx, Zpy, Abs, Abx, Aby, Izx, Izy, Izp, Rel, Bbx,
    Brk, Jsr, Rti, Rts, Push, Pull, JmpAbs, JmpInd, JmpIndX,
    Nop1, Nop2, NopZp, NopZpx, NopAbs, Nop5C, Wai, Stp,
};

enum class Op : uint8_t {
    // reads
    LDA, LDX, LDY, EOR, AND, ORA, ADC, SBC, CMP, CPX, CPY, BIT, BITI, NOP,
    // writes
    STA, STX, STY, STZ,
    // read-modify-write
    ASL, LSR, ROL, ROR, INC, DEC, TSB, TRB, RMB, SMB,
    // implied
    TAX, TAY, TXA, TYA, TSX, TXS, INX, INY, DEX, DEY, CLC, SEC, CLI, SEI, CLV, CLD, SED, INA, DEA,
    // branches (BBR/BBS use BBR/BBS)
    BPL, BMI, BVC, BVS, BCC, BCS, BNE, BEQ, BRA, BBR, BBS,
    // stack registers (Push/Pull)
    PHA, PHP, PHX, PHY, PLA, PLP, PLX, PLY,
    SPECIAL,
};

enum class Kind : uint8_t { Read, Write, Modify, Other };

constexpr Kind kindOf(Op op) {
    if (op <= Op::NOP) return Kind::Read;
    if (op <= Op::STZ) return Kind::Write;
    if (op <= Op::SMB) return Kind::Modify;
    return Kind::Other;
}

struct Entry { Mode mode; Op op; };

// Table 5-2, row by row ($x0..$xF). The WDC-only WAI ($CB) and STP ($DB) are listed here;
// the Rockwell variant turns them into one-byte NOPs.
#define E(m, o) Entry{Mode::m, Op::o}
inline constexpr Entry kOpcodes[256] = {
    E(Brk,SPECIAL),E(Izx,ORA),E(Nop2,NOP),E(Nop1,NOP),E(Zp,TSB), E(Zp,ORA), E(Zp,ASL), E(Zp,RMB),
    E(Push,PHP),  E(Imm,ORA),E(Acc,ASL),E(Nop1,NOP),E(Abs,TSB),E(Abs,ORA),E(Abs,ASL),E(Bbx,BBR),
    E(Rel,BPL),   E(Izy,ORA),E(Izp,ORA),E(Nop1,NOP),E(Zp,TRB), E(Zpx,ORA),E(Zpx,ASL),E(Zp,RMB),
    E(Imp,CLC),   E(Aby,ORA),E(Acc,INA),E(Nop1,NOP),E(Abs,TRB),E(Abx,ORA),E(Abx,ASL),E(Bbx,BBR),
    E(Jsr,SPECIAL),E(Izx,AND),E(Nop2,NOP),E(Nop1,NOP),E(Zp,BIT), E(Zp,AND), E(Zp,ROL), E(Zp,RMB),
    E(Pull,PLP),  E(Imm,AND),E(Acc,ROL),E(Nop1,NOP),E(Abs,BIT),E(Abs,AND),E(Abs,ROL),E(Bbx,BBR),
    E(Rel,BMI),   E(Izy,AND),E(Izp,AND),E(Nop1,NOP),E(Zpx,BIT),E(Zpx,AND),E(Zpx,ROL),E(Zp,RMB),
    E(Imp,SEC),   E(Aby,AND),E(Acc,DEA),E(Nop1,NOP),E(Abx,BIT),E(Abx,AND),E(Abx,ROL),E(Bbx,BBR),
    E(Rti,SPECIAL),E(Izx,EOR),E(Nop2,NOP),E(Nop1,NOP),E(NopZp,NOP),E(Zp,EOR),E(Zp,LSR), E(Zp,RMB),
    E(Push,PHA),  E(Imm,EOR),E(Acc,LSR),E(Nop1,NOP),E(JmpAbs,SPECIAL),E(Abs,EOR),E(Abs,LSR),E(Bbx,BBR),
    E(Rel,BVC),   E(Izy,EOR),E(Izp,EOR),E(Nop1,NOP),E(NopZpx,NOP),E(Zpx,EOR),E(Zpx,LSR),E(Zp,RMB),
    E(Imp,CLI),   E(Aby,EOR),E(Push,PHY),E(Nop1,NOP),E(Nop5C,NOP),E(Abx,EOR),E(Abx,LSR),E(Bbx,BBR),
    E(Rts,SPECIAL),E(Izx,ADC),E(Nop2,NOP),E(Nop1,NOP),E(Zp,STZ), E(Zp,ADC), E(Zp,ROR), E(Zp,RMB),
    E(Pull,PLA),  E(Imm,ADC),E(Acc,ROR),E(Nop1,NOP),E(JmpInd,SPECIAL),E(Abs,ADC),E(Abs,ROR),E(Bbx,BBR),
    E(Rel,BVS),   E(Izy,ADC),E(Izp,ADC),E(Nop1,NOP),E(Zpx,STZ),E(Zpx,ADC),E(Zpx,ROR),E(Zp,RMB),
    E(Imp,SEI),   E(Aby,ADC),E(Pull,PLY),E(Nop1,NOP),E(JmpIndX,SPECIAL),E(Abx,ADC),E(Abx,ROR),E(Bbx,BBR),
    E(Rel,BRA),   E(Izx,STA),E(Nop2,NOP),E(Nop1,NOP),E(Zp,STY), E(Zp,STA), E(Zp,STX), E(Zp,SMB),
    E(Imp,DEY),   E(Imm,BITI),E(Imp,TXA),E(Nop1,NOP),E(Abs,STY),E(Abs,STA),E(Abs,STX),E(Bbx,BBS),
    E(Rel,BCC),   E(Izy,STA),E(Izp,STA),E(Nop1,NOP),E(Zpx,STY),E(Zpx,STA),E(Zpy,STX),E(Zp,SMB),
    E(Imp,TYA),   E(Aby,STA),E(Imp,TXS),E(Nop1,NOP),E(Abs,STZ),E(Abx,STA),E(Abx,STZ),E(Bbx,BBS),
    E(Imm,LDY),   E(Izx,LDA),E(Imm,LDX),E(Nop1,NOP),E(Zp,LDY), E(Zp,LDA), E(Zp,LDX), E(Zp,SMB),
    E(Imp,TAY),   E(Imm,LDA),E(Imp,TAX),E(Nop1,NOP),E(Abs,LDY),E(Abs,LDA),E(Abs,LDX),E(Bbx,BBS),
    E(Rel,BCS),   E(Izy,LDA),E(Izp,LDA),E(Nop1,NOP),E(Zpx,LDY),E(Zpx,LDA),E(Zpy,LDX),E(Zp,SMB),
    E(Imp,CLV),   E(Aby,LDA),E(Imp,TSX),E(Nop1,NOP),E(Abx,LDY),E(Abx,LDA),E(Aby,LDX),E(Bbx,BBS),
    E(Imm,CPY),   E(Izx,CMP),E(Nop2,NOP),E(Nop1,NOP),E(Zp,CPY), E(Zp,CMP), E(Zp,DEC), E(Zp,SMB),
    E(Imp,INY),   E(Imm,CMP),E(Imp,DEX),E(Wai,SPECIAL),E(Abs,CPY),E(Abs,CMP),E(Abs,DEC),E(Bbx,BBS),
    E(Rel,BNE),   E(Izy,CMP),E(Izp,CMP),E(Nop1,NOP),E(NopZpx,NOP),E(Zpx,CMP),E(Zpx,DEC),E(Zp,SMB),
    E(Imp,CLD),   E(Aby,CMP),E(Push,PHX),E(Stp,SPECIAL),E(NopAbs,NOP),E(Abx,CMP),E(Abx,DEC),E(Bbx,BBS),
    E(Imm,CPX),   E(Izx,SBC),E(Nop2,NOP),E(Nop1,NOP),E(Zp,CPX), E(Zp,SBC), E(Zp,INC), E(Zp,SMB),
    E(Imp,INX),   E(Imm,SBC),E(Imp,NOP),E(Nop1,NOP),E(Abs,CPX),E(Abs,SBC),E(Abs,INC),E(Bbx,BBS),
    E(Rel,BEQ),   E(Izy,SBC),E(Izp,SBC),E(Nop1,NOP),E(NopZpx,NOP),E(Zpx,SBC),E(Zpx,INC),E(Zp,SMB),
    E(Imp,SED),   E(Aby,SBC),E(Pull,PLX),E(Nop1,NOP),E(NopAbs,NOP),E(Abx,SBC),E(Abx,INC),E(Bbx,BBS),
};
#undef E

constexpr uint8_t kAccess = 20;   // step of the data access once the address is known

} // namespace c02

enum class C02Variant : uint8_t { Rockwell, Wdc };

template <class Bus>
class Cpu65C02 {
public:
#pragma pack(push, 1)
    struct State {
        CpuRegs r;
        uint8_t op = 0;
        uint8_t t = 0;          // step within the instruction; 0 = opcode fetch
        uint8_t data = 0;
        uint8_t data2 = 0;
        uint16_t ea = 0;
        uint16_t ptr = 0;
        bool cross = false;
        bool interrupt = false; // the sequence being run is IRQ/NMI/reset, not BRK
        bool reset = false;
        bool takeInterrupt = false;
        bool waiting = false;   // WAI: stopped until an interrupt line is asserted
        bool stopped = false;   // STP: stopped until reset
    };
    struct Snapshot {
        State st;
        bool irq, nmiLine, nmiSeen, nmiPending, poll;
    };
#pragma pack(pop)

    explicit Cpu65C02(Bus& bus, C02Variant v = C02Variant::Rockwell) : bus_(bus), variant_(v) {}

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

    void tick() {
        if (nmiLine_ && !nmiSeen_) nmiPending_ = true;
        nmiSeen_ = nmiLine_;
        poll_ = nmiPending_ || (irq_ && !(st_.r.p & flag::I));
        step();
    }

    Snapshot snapshot() const { return {st_, irq_, nmiLine_, nmiSeen_, nmiPending_, poll_}; }
    void restore(const Snapshot& s) {
        st_ = s.st; irq_ = s.irq; nmiLine_ = s.nmiLine; nmiSeen_ = s.nmiSeen; nmiPending_ = s.nmiPending; poll_ = s.poll;
    }

    CpuRegs& regs() { return st_.r; }
    const CpuRegs& regs() const { return st_.r; }
    State& state() { return st_; }
    const State& state() const { return st_; }
    bool atInstructionStart() const { return st_.t == 0; }
    bool jammed() const { return st_.stopped; }   // (the test runner's name for "will not go on")
    bool waiting() const { return st_.waiting; }

private:
    using Mode = c02::Mode;
    using Op = c02::Op;
    using Kind = c02::Kind;
    static constexpr uint8_t kAccess = c02::kAccess;

    uint8_t rd(uint16_t a) { return bus_.read(a); }
    void wr(uint16_t a, uint8_t v) { bus_.write(a, v); }
    uint16_t sp() const { return static_cast<uint16_t>(0x100 | st_.r.s); }
    void setNZ(uint8_t v) {
        st_.r.p = static_cast<uint8_t>((st_.r.p & ~(flag::N | flag::Z)) | (v & flag::N) | (v ? 0 : flag::Z));
    }
    void setFlag(uint8_t f, bool on) { st_.r.p = static_cast<uint8_t>(on ? (st_.r.p | f) : (st_.r.p & ~f)); }
    void finish() { st_.t = 0; st_.takeInterrupt = poll_; }

    Mode modeOf(uint8_t op) const {
        if (variant_ == C02Variant::Rockwell) {
            // WDC's WAI and STP are a plain two-cycle NOP and a zp,X NOP here, and $5C is as
            // long as $DC/$FC (bus traces).
            if (op == 0xCB) return Mode::Imp;
            if (op == 0xDB) return Mode::NopZpx;
            if (op == 0x5C) return Mode::NopAbs;
        }
        return c02::kOpcodes[op].mode;
    }

    // ---- ALU ---------------------------------------------------------------
    // Decimal mode: the result is corrected per digit, and N, V, Z are those of the
    // corrected result (V as the binary sum's) (data sheet table 7-1).
    void adc(uint8_t v) {
        auto& r = st_.r;
        const unsigned c = r.p & flag::C;
        if (r.p & flag::D) {
            unsigned lo = (r.a & 0x0F) + (v & 0x0F) + c;
            if (lo > 9) lo += 6;
            unsigned sum = (r.a & 0xF0) + (v & 0xF0) + (lo > 0x0F ? 0x10 : 0) + (lo & 0x0F);
            setFlag(flag::V, (~(r.a ^ v) & (r.a ^ sum) & 0x80) != 0);
            if (sum > 0x9F) sum += 0x60;
            setFlag(flag::C, sum > 0xFF);
            r.a = static_cast<uint8_t>(sum);
            setNZ(r.a);
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
        setFlag(flag::C, diff < 0x100);
        setFlag(flag::V, ((r.a ^ v) & (r.a ^ diff) & 0x80) != 0);
        if (r.p & flag::D) {
            int lo = (r.a & 0x0F) - (v & 0x0F) - static_cast<int>(borrow);
            int res = static_cast<int>(r.a) - v - static_cast<int>(borrow);
            if (res < 0) res -= 0x60;
            if (lo < 0) res -= 0x06;
            r.a = static_cast<uint8_t>(res);
        } else {
            r.a = static_cast<uint8_t>(diff);
        }
        setNZ(r.a);
    }
    void compare(uint8_t reg, uint8_t v) {
        setFlag(flag::C, reg >= v);
        setNZ(static_cast<uint8_t>(reg - v));
    }
    bool decimalExtra(Op op) const { return (op == Op::ADC || op == Op::SBC) && (st_.r.p & flag::D); }

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
        case Op::BITI: setFlag(flag::Z, (r.a & v) == 0); break;   // immediate: Z only
        default: break;
        }
    }
    uint8_t storeValue(Op op) const {
        switch (op) {
        case Op::STA: return st_.r.a;
        case Op::STX: return st_.r.x;
        case Op::STY: return st_.r.y;
        default: return 0;   // STZ
        }
    }
    uint8_t execModify(Op op, uint8_t v) {
        auto& r = st_.r;
        uint8_t o = v;
        const uint8_t bit = static_cast<uint8_t>(1u << ((st_.op >> 4) & 7));
        switch (op) {
        case Op::ASL: setFlag(flag::C, v & 0x80); o = static_cast<uint8_t>(v << 1); setNZ(o); break;
        case Op::LSR: setFlag(flag::C, v & 1); o = static_cast<uint8_t>(v >> 1); setNZ(o); break;
        case Op::ROL: { const uint8_t c = r.p & flag::C; setFlag(flag::C, v & 0x80); o = static_cast<uint8_t>((v << 1) | c); setNZ(o); break; }
        case Op::ROR: { const uint8_t c = (r.p & flag::C) ? 0x80 : 0; setFlag(flag::C, v & 1); o = static_cast<uint8_t>((v >> 1) | c); setNZ(o); break; }
        case Op::INC: case Op::INA: o = static_cast<uint8_t>(v + 1); setNZ(o); break;
        case Op::DEC: case Op::DEA: o = static_cast<uint8_t>(v - 1); setNZ(o); break;
        case Op::TSB: setFlag(flag::Z, (r.a & v) == 0); o = v | r.a; break;
        case Op::TRB: setFlag(flag::Z, (r.a & v) == 0); o = static_cast<uint8_t>(v & ~r.a); break;
        case Op::RMB: o = static_cast<uint8_t>(v & ~bit); break;
        case Op::SMB: o = v | bit; break;
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
        case Op::BRA: return true;
        default: return false;
        }
    }

    // Indexed: the low byte is indexed; a carry into the high byte costs a cycle, which reads
    // the instruction's last byte again (table 7-1).
    void index(uint8_t lo, uint8_t hi, uint8_t idx) {
        const unsigned sum = lo + idx;
        st_.cross = sum > 0xFF;
        st_.ea = static_cast<uint16_t>(((hi << 8) | lo) + idx);
    }
    // After the address of an indexed mode is formed (pc points past the instruction).
    void afterIndex(Op op) {
        const Kind k = c02::kindOf(op);
        // A read takes the extra cycle only on a crossing; writes always do. Read-modify-write
        // in the same page skips it (table 7-1: six cycles, absolute indexed).
        // INC and DEC absolute,X take it always (bus traces).
        const bool incDec = op == Op::INC || op == Op::DEC;
        if (st_.cross || k == Kind::Write || (k == Kind::Modify && incDec)) st_.t = kAccess - 1;
        else st_.t = kAccess;
    }

    void step() {
        auto& r = st_.r;
        if (st_.stopped) { rd(r.pc); return; }
        if (st_.waiting) {
            // WAI: holds until IRQ or NMI is asserted (even with I set: then it just goes on).
            rd(r.pc);
            if (nmiPending_ || irq_) { st_.waiting = false; finish(); }
            return;
        }
        if (st_.t == 0) {
            if (st_.takeInterrupt) {
                rd(r.pc);
                st_.op = 0x00;
                st_.interrupt = true;
            } else {
                st_.op = rd(r.pc++);
                st_.interrupt = false;
                // A one-byte NOP is over with its fetch: one cycle (table 7-1).
                if (modeOf(st_.op) == Mode::Nop1) { finish(); return; }
            }
            st_.t = 1;
            return;
        }

        const c02::Entry e = c02::kOpcodes[st_.op];
        const Mode mode = modeOf(st_.op);
        if (st_.t == kAccess - 1) {
            // The extra cycle of an indexed mode: the last instruction byte read again.
            rd(static_cast<uint16_t>(r.pc - 1));
            st_.t = kAccess;
            return;
        }
        if (st_.t >= kAccess) { access(e.op); return; }

        switch (mode) {
        case Mode::Imp:
            rd(r.pc);
            execImplied(e.op);
            finish();
            return;
        case Mode::Acc:
            rd(r.pc);
            if (e.op == Op::INA) r.a = execModify(Op::INC, r.a);
            else if (e.op == Op::DEA) r.a = execModify(Op::DEC, r.a);
            else r.a = execModify(e.op, r.a);
            finish();
            return;
        case Mode::Imm:
            if (st_.t == 1) {
                st_.data = rd(r.pc++);
                execRead(e.op, st_.data);
                if (decimalExtra(e.op)) { st_.t = kAccess + 5; return; }
                finish();
                return;
            }
            return;
        case Mode::Zp:
            st_.ea = rd(r.pc++);
            st_.t = kAccess;
            return;
        case Mode::Zpx:
        case Mode::Zpy:
            if (st_.t == 1) { st_.ptr = rd(r.pc++); st_.t = 2; return; }
            rd(st_.ptr);   // the unindexed zero-page address, as on the NMOS chip
            st_.ea = static_cast<uint8_t>(st_.ptr + (mode == Mode::Zpx ? r.x : r.y));
            st_.t = kAccess;
            return;
        case Mode::Abs:
            if (st_.t == 1) { st_.ea = rd(r.pc++); st_.t = 2; return; }
            st_.ea = static_cast<uint16_t>(st_.ea | (rd(r.pc++) << 8));
            st_.t = kAccess;
            return;
        case Mode::Abx:
        case Mode::Aby:
            if (st_.t == 1) { st_.data = rd(r.pc++); st_.t = 2; return; }
            index(st_.data, rd(r.pc++), mode == Mode::Abx ? r.x : r.y);
            afterIndex(e.op);
            return;
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
            default:
                index(st_.data, rd(static_cast<uint8_t>(st_.ptr + 1)), r.y);
                afterIndex(e.op);
                return;
            }
        case Mode::Izp:
            switch (st_.t) {
            case 1: st_.ptr = rd(r.pc++); st_.t = 2; return;
            case 2: st_.ea = rd(st_.ptr); st_.t = 3; return;
            default:
                st_.ea = static_cast<uint16_t>(st_.ea | (rd(static_cast<uint8_t>(st_.ptr + 1)) << 8));
                st_.t = kAccess;
                return;
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
                st_.cross = (target & 0xFF00) != (r.pc & 0xFF00);
                st_.ea = target;
                if (!st_.cross) { r.pc = target; finish(); return; }
                st_.t = 3;
                return;
            }
            default:
                // Crossing a page: the address before the carry into the high byte (as NMOS).
                rd(static_cast<uint16_t>((r.pc & 0xFF00) | (st_.ea & 0xFF)));
                r.pc = st_.ea;
                finish();
                return;
            }
        case Mode::Bbx:
            // BBR/BBS zp,rel: zero-page address, the byte read (twice), the offset, then a
            // branch as above.
            switch (st_.t) {
            case 1: st_.ea = rd(r.pc++); st_.t = 2; return;
            case 2: st_.data2 = rd(st_.ea); st_.t = 3; return;
            case 3: rd(st_.ea); st_.t = 4; return;
            case 4: {
                st_.data = rd(r.pc++);
                const bool set = (st_.data2 >> ((st_.op >> 4) & 7)) & 1;
                if (set == (e.op == Op::BBS)) st_.t = 5;
                else finish();
                return;
            }
            case 5: {
                rd(r.pc);
                const uint16_t target = static_cast<uint16_t>(r.pc + static_cast<int8_t>(st_.data));
                st_.cross = (target & 0xFF00) != (r.pc & 0xFF00);
                st_.ea = target;
                if (!st_.cross) { r.pc = target; finish(); return; }
                st_.t = 6;
                return;
            }
            default:
                rd(r.pc);   // unlike a plain branch, the next opcode's address again (bus traces)
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
        case Mode::Push:
            if (st_.t == 1) { rd(r.pc); st_.t = 2; return; }
            switch (e.op) {
            case Op::PHA: wr(sp(), r.a); break;
            case Op::PHX: wr(sp(), r.x); break;
            case Op::PHY: wr(sp(), r.y); break;
            default: wr(sp(), static_cast<uint8_t>(r.p | flag::B | flag::U)); break;
            }
            --r.s;
            finish();
            return;
        case Mode::Pull:
            switch (st_.t) {
            case 1: rd(r.pc); st_.t = 2; return;
            case 2: rd(sp()); ++r.s; st_.t = 3; return;
            default: {
                const uint8_t v = rd(sp());
                switch (e.op) {
                case Op::PLA: r.a = v; setNZ(v); break;
                case Op::PLX: r.x = v; setNZ(v); break;
                case Op::PLY: r.y = v; setNZ(v); break;
                default: r.p = static_cast<uint8_t>((v & ~flag::B) | flag::U); break;
                }
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
        case Mode::JmpIndX:
            switch (st_.t) {
            case 1: st_.ptr = rd(r.pc++); st_.t = 2; return;
            case 2: st_.ptr = static_cast<uint16_t>(st_.ptr | (rd(r.pc) << 8)); st_.t = 3; return;
            case 3:
                if (mode == Mode::JmpIndX) {
                    // (abs,X): the low operand byte read again while X is added.
                    rd(static_cast<uint16_t>(r.pc - 1));
                    st_.ptr = static_cast<uint16_t>(st_.ptr + r.x);
                    st_.t = 4;
                    return;
                }
                st_.data = rd(st_.ptr);
                st_.t = 5;
                return;
            case 4: st_.data = rd(st_.ptr); st_.t = 5; return;
            case 5:
                if (mode == Mode::JmpIndX) {
                    r.pc = static_cast<uint16_t>(st_.data | (rd(static_cast<uint16_t>(st_.ptr + 1)) << 8));
                    finish();
                    return;
                }
                // (ind): first the NMOS chip's address (no carry into the pointer's high byte),
                // then the extra cycle reads the carried one, which counts (table 7-1).
                rd(static_cast<uint16_t>((st_.ptr & 0xFF00) | ((st_.ptr + 1) & 0xFF)));
                st_.t = 6;
                return;
            default:
                r.pc = static_cast<uint16_t>(st_.data | (rd(static_cast<uint16_t>(st_.ptr + 1)) << 8));
                finish();
                return;
            }
        case Mode::Nop1:
            finish();   // the fetch was the whole instruction
            return;
        case Mode::Nop2:
            rd(r.pc++);
            finish();
            return;
        case Mode::NopZp:
            if (st_.t == 1) { st_.ea = rd(r.pc++); st_.t = 2; return; }
            rd(st_.ea);
            finish();
            return;
        case Mode::NopZpx:
            switch (st_.t) {
            case 1: st_.ptr = rd(r.pc++); st_.t = 2; return;
            case 2: rd(st_.ptr); st_.t = 3; return;
            default: rd(static_cast<uint8_t>(st_.ptr + r.x)); finish(); return;
            }
        case Mode::NopAbs:
            switch (st_.t) {
            case 1: st_.data = rd(r.pc++); st_.t = 2; return;
            case 2: rd(r.pc++); st_.t = 3; return;
            default: rd(static_cast<uint16_t>(r.pc - 1)); finish(); return;
            }
        case Mode::Nop5C:
            // WDC: eight cycles (data sheet table 7-1; the per-opcode traces show four, but they
            // come from an emulator and the data sheet outranks them, hw/cpu65c02.md). Which
            // addresses its last five cycles read is not documented: $FFxx here, unverified.
            switch (st_.t) {
            case 1: st_.data = rd(r.pc++); st_.t = 2; return;
            case 2: st_.ea = static_cast<uint16_t>(0xFF00 | st_.data); rd(r.pc++); st_.t = 3; return;
            default:
                rd(st_.ea);
                if (st_.t == 7) finish(); else ++st_.t;
                return;
            }
        case Mode::Wai:
            if (st_.t == 1) { rd(r.pc); st_.t = 2; return; }
            rd(r.pc);
            st_.waiting = true;
            st_.t = 0;
            return;
        case Mode::Stp:
            if (st_.t == 1) { rd(r.pc); st_.t = 2; return; }
            rd(r.pc);
            st_.stopped = true;
            st_.t = 0;
            return;
        }
    }

    void access(Op op) {
        const uint8_t sub = static_cast<uint8_t>(st_.t - kAccess);
        switch (c02::kindOf(op)) {
        case Kind::Read:
            if (sub == 0) {
                execRead(op, rd(st_.ea));
                if (decimalExtra(op)) { st_.t = kAccess + 1; return; }
                finish();
                return;
            }
            // Decimal ADC/SBC: one more cycle (table 7-1).
            // After an immediate operand the address bus shows a fixed address: for ADC $0059
            // (Rockwell) or $007F (WDC), for SBC $0000 (every decimal case of the bus traces).
            rd(st_.t == kAccess + 5 ? (op != Op::ADC ? 0x0000 : variant_ == C02Variant::Wdc ? 0x007F : 0x0059) : st_.ea);
            finish();
            return;
        case Kind::Write:
            wr(st_.ea, storeValue(op));
            finish();
            return;
        case Kind::Modify:
            // Two reads and one write (table 7-1).
            switch (sub) {
            case 0: st_.data = rd(st_.ea); st_.t = kAccess + 1; return;
            case 1: rd(st_.ea); st_.data = execModify(op, st_.data); st_.t = kAccess + 2; return;
            default: wr(st_.ea, st_.data); finish(); return;
            }
        case Kind::Other:
            finish();
            return;
        }
    }

    // BRK, IRQ, NMI and reset; all of them clear D (table 7-1).
    void interruptSequence() {
        auto& r = st_.r;
        const bool hw = st_.interrupt;
        switch (st_.t) {
        case 1:
            rd(r.pc);
            if (!hw) ++r.pc;
            st_.t = 2;
            return;
        case 2: push(static_cast<uint8_t>(r.pc >> 8)); st_.t = 3; return;
        case 3: push(static_cast<uint8_t>(r.pc)); st_.t = 4; return;
        case 4: {
            const uint8_t pushed = static_cast<uint8_t>((r.p | flag::U | (hw ? 0 : flag::B)) & (hw ? ~flag::B : 0xFF));
            if (st_.reset) st_.ea = 0xFFFC;
            else if (hw && nmiPending_) { st_.ea = 0xFFFA; nmiPending_ = false; }
            else st_.ea = 0xFFFE;   // BRK keeps its own vector (table 7-1)
            push(pushed);
            st_.t = 5;
            return;
        }
        case 5:
            st_.data = rd(st_.ea);
            r.p = static_cast<uint8_t>((r.p | flag::I) & ~flag::D);
            st_.t = 6;
            return;
        default:
            r.pc = static_cast<uint16_t>(st_.data | (rd(static_cast<uint16_t>(st_.ea + 1)) << 8));
            st_.reset = false;
            st_.interrupt = false;
            st_.t = 0;
            st_.takeInterrupt = false;
            return;
        }
    }
    void push(uint8_t v) {
        if (st_.reset) rd(sp());
        else wr(sp(), v);
        --st_.r.s;
    }

    Bus& bus_;
    C02Variant variant_;
    State st_;
    bool irq_ = false, nmiLine_ = false, nmiSeen_ = false, nmiPending_ = false, poll_ = false;
};

} // namespace k64
