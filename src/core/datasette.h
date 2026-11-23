// The datasette on the cassette port: its buttons, its motor (switched by the 6510 port) and
// the tape moving past the head. Reading: each pulse's end is a falling edge on the read line,
// CIA 1's FLAG. Recording: the write line's period between rising edges becomes a pulse.
// Rules and sources: local-notes/hw/tape.md.
#pragma once

#include "tap.h"

#include <cstdint>
#include <memory>

namespace k64 {

class Datasette {
public:
    enum class Button : uint8_t { None, Play, Record };   // Record = RECORD and PLAY down together

    void insert(std::shared_ptr<Tape> tape) { tape_ = std::move(tape); s_ = State{}; }
    void eject() { tape_.reset(); s_ = State{}; }
    bool hasTape() const { return tape_ != nullptr; }
    void press(Button b) { s_.button = static_cast<uint8_t>(tape_ ? b : Button::None); }
    void stop() { s_.button = static_cast<uint8_t>(Button::None); }
    void rewind() { stop(); s_.pulse = 0; s_.left = 0; }
    Button button() const { return static_cast<Button>(s_.button); }

    // D1: the sense line (6510 port bit 4) is low while PLAY is down.
    bool senseLow() const { return s_.button != static_cast<uint8_t>(Button::None); }
    // One machine cycle. motorOn: the port drives bit 5 low (D2). writeLine: port bit 3.
    // Returns the read line's level for CIA 1's FLAG (low for one cycle at a pulse's end).
    bool clock(bool motorOn, bool writeLine);
    size_t position() const { return s_.pulse; }

#pragma pack(push, 1)
    struct State {
        uint8_t button = 0;
        uint32_t pulse = 0;      // the pulse under the head
        uint32_t left = 0;       // cycles of it still to pass (0: start it)
        uint8_t writeLevel = 0;  // the write line as last seen
        uint32_t since = 0;      // cycles since its last rising edge while recording
        uint8_t started = 0;     // a rising edge has been seen since recording began
    };
#pragma pack(pop)
    const State& state() const { return s_; }
    void setState(const State& s) { s_ = s; }

private:
    std::shared_ptr<Tape> tape_;
    State s_;
};

} // namespace k64
