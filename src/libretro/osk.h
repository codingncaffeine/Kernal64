// The on-screen keyboard: the C64's keyboard drawn over the picture, worked with a RetroPad
// alone. D-pad moves, B presses the key under the cursor (held while B is), X = SPACE,
// Start = RETURN, Y = SHIFT LOCK, A moves the keyboard to the other half of the picture.
// SHIFT, C= and CTRL stay down after a press until the next key is let go.
// Labels are drawn with the machine's own character ROM. Input state only: nothing of it is
// in a save state.
#pragma once

#include "keys.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace k64lr {

class Osk {
public:
    struct Pad { bool up, down, left, right, press, space, ret, lock, move; };

    void setFont(const uint8_t* chargen);   // 4096-byte character ROM (first half used); null: none
    bool isOpen() const { return open_; }
    void setOpen(bool open);
    void update(const Pad& pad);            // once a frame while open
    // The keys it holds down this frame (indexes into kC64Keys) and RESTORE.
    void keysDown(std::array<bool, kC64KeyCount>& down) const;
    // Drawn over a w x h picture whose rows are `pitch` pixels apart.
    void draw(uint32_t* picture, size_t pitch, int w, int h) const;

    // Its state goes into save states (a replay with the same pad input must come out the
    // same; with run-ahead the cursor would otherwise move once per hidden frame).
    static constexpr size_t kStateSize = 14 * 4 + kC64KeyCount;
    void saveState(uint8_t* out) const;
    void loadState(const uint8_t* in);

private:
    struct Slot { int row, index; };
    int keyAt(int row, int index) const;     // kC64Keys index, or -2 for SHIFT LOCK
    void moveVertically(int dir);

    std::array<uint8_t, 2048> font_{};
    bool haveFont_ = false;
    bool open_ = false;
    bool top_ = false;                 // the keyboard sits in the top half
    Slot sel_{2, 5};
    int held_ = -1;                    // key held by B (kC64Keys index), -1 none
    int heldFrames_ = 0;
    bool pressWas_ = false, lockWas_ = false, moveWas_ = false;
    int repeatDir_ = -1, repeatFrames_ = 0;
    bool shiftLock_ = false;
    std::array<bool, kC64KeyCount> latched_{};   // SHIFT, C=, CTRL waiting for the next key
    bool space_ = false, ret_ = false;
};

} // namespace k64lr
