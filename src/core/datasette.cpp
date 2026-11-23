#include "datasette.h"

namespace k64 {

bool Datasette::clock(bool motorOn, bool writeLine) {
    if (!tape_ || !motorOn) {
        s_.writeLevel = writeLine ? 1 : 0;
        return true;
    }
    const auto b = static_cast<Button>(s_.button);
    if (b == Button::Record) {
        // D4: a pulse = the write line's period, from one rising edge to the next. Recording
        // replaces what follows the head.
        const bool rising = writeLine && !s_.writeLevel;
        s_.writeLevel = writeLine ? 1 : 0;
        ++s_.since;
        if (rising) {
            if (s_.started) {
                auto& p = tape_->pulses;
                if (s_.pulse < p.size()) p.resize(s_.pulse);
                p.push_back(s_.since);
                ++s_.pulse;
                tape_->changed = true;
            }
            s_.started = 1;
            s_.since = 0;
        }
        return true;
    }
    s_.writeLevel = writeLine ? 1 : 0;
    s_.started = 0;
    if (b != Button::Play || s_.pulse >= tape_->pulses.size()) return true;
    // D3: playing, each pulse's end pulls the read line low for a cycle (CIA 1's FLAG).
    if (s_.left == 0) s_.left = tape_->pulses[s_.pulse];
    if (--s_.left == 0) {
        ++s_.pulse;
        return false;
    }
    return true;
}

} // namespace k64
