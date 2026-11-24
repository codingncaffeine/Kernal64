#include "osk.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace k64lr {

namespace {

constexpr int kLock = -2;   // SHIFT LOCK: holds the left SHIFT down until pressed again

// A label: text in the character ROM's upper-case set; \x1c pound, \x1e up arrow, \x1f left
// arrow (their screen codes), \x01 down arrow and \x02 right arrow (drawn here: the ROM has none).
struct KeyDef { const char* label; const char* id; bool fkey; };
const std::vector<std::vector<KeyDef>>& rows() {
    static const std::vector<std::vector<KeyDef>> r = {
        {{"\x1f", "leftarrow", false}, {"1", "1", false}, {"2", "2", false}, {"3", "3", false}, {"4", "4", false},
         {"5", "5", false}, {"6", "6", false}, {"7", "7", false}, {"8", "8", false}, {"9", "9", false}, {"0", "0", false},
         {"+", "plus", false}, {"-", "minus", false}, {"\x1c", "pound", false}, {"CLR", "clrhome", false},
         {"DEL", "insdel", false}, {"F1", "f1", true}},
        {{"CTRL", "ctrl", false}, {"Q", "q", false}, {"W", "w", false}, {"E", "e", false}, {"R", "r", false},
         {"T", "t", false}, {"Y", "y", false}, {"U", "u", false}, {"I", "i", false}, {"O", "o", false}, {"P", "p", false},
         {"@", "at", false}, {"*", "star", false}, {"\x1e", "uparrow", false}, {"RSTR", "restore", false}, {"F3", "f3", true}},
        {{"STOP", "runstop", false}, {"LOCK", nullptr, false}, {"A", "a", false}, {"S", "s", false}, {"D", "d", false},
         {"F", "f", false}, {"G", "g", false}, {"H", "h", false}, {"J", "j", false}, {"K", "k", false}, {"L", "l", false},
         {":", "colon", false}, {";", "semicolon", false}, {"=", "equals", false}, {"RET", "return", false}, {"F5", "f5", true}},
        {{"C=", "cbm", false}, {"SHFT", "lshift", false}, {"Z", "z", false}, {"X", "x", false}, {"C", "c", false},
         {"V", "v", false}, {"B", "b", false}, {"N", "n", false}, {"M", "m", false}, {",", "comma", false},
         {".", "period", false}, {"/", "slash", false}, {"SHFT", "rshift", false}, {"\x01", "crsr_down", false},
         {"\x02", "crsr_right", false}, {"F7", "f7", true}},
        {{"SPACE", "space", false}},
    };
    return r;
}

constexpr int kKeyH = 13, kGap = 2, kMargin = 4;
constexpr uint8_t kDownArrow[8] = {0x18, 0x18, 0x18, 0x18, 0x7E, 0x3C, 0x18, 0x00};
constexpr uint8_t kRightArrow[8] = {0x00, 0x08, 0x0C, 0x7E, 0x0C, 0x08, 0x00, 0x00};

int keyWidth(const KeyDef& k) {
    if (!std::strcmp(k.label, "SPACE")) return 120;
    const int n = static_cast<int>(std::strlen(k.label));
    return std::max(12, 8 * n + 4);
}

struct Rect { int x, y, w; };
// Key rectangles relative to the panel; the panel's size.
struct Layout { std::vector<std::vector<Rect>> keys; int w, h; };
const Layout& layout() {
    static Layout l;
    if (!l.keys.empty()) return l;
    const auto& r = rows();
    int width = 0;
    for (const auto& row : r) {
        int x = 0;
        for (const KeyDef& k : row) x += keyWidth(k) + kGap + (k.fkey ? 8 : 0);
        width = std::max(width, x - kGap);
    }
    l.w = width + 2 * kMargin;
    l.h = static_cast<int>(r.size()) * (kKeyH + kGap) - kGap + 2 * kMargin;
    for (size_t ri = 0; ri < r.size(); ++ri) {
        std::vector<Rect> out;
        const int y = kMargin + static_cast<int>(ri) * (kKeyH + kGap);
        int x = kMargin;
        if (r[ri].size() == 1) x = (l.w - keyWidth(r[ri][0])) / 2;
        for (const KeyDef& k : r[ri]) {
            const int w = keyWidth(k);
            if (k.fkey) x = l.w - kMargin - w;   // the function keys stand in a column of their own
            out.push_back({x, y, w});
            x += w + kGap;
        }
        l.keys.push_back(out);
    }
    return l;
}

uint32_t blend(uint32_t under, uint32_t over, int overParts, int parts) {
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8) {
        const uint32_t a = (under >> s) & 0xFF, b = (over >> s) & 0xFF;
        out |= ((a * static_cast<uint32_t>(parts - overParts) + b * static_cast<uint32_t>(overParts)) / static_cast<uint32_t>(parts)) << s;
    }
    return out;
}

} // namespace

void Osk::saveState(uint8_t* out) const {
    const int32_t v[14] = {open_, top_, sel_.row, sel_.index, held_, heldFrames_, pressWas_, lockWas_,
                           moveWas_, repeatDir_, repeatFrames_, shiftLock_, space_, ret_};
    for (int32_t x : v)
        for (int b = 0; b < 4; ++b) *out++ = static_cast<uint8_t>(static_cast<uint32_t>(x) >> (8 * b));
    for (bool l : latched_) *out++ = l ? 1 : 0;
}

void Osk::loadState(const uint8_t* in) {
    int32_t v[14];
    for (int32_t& x : v) {
        x = static_cast<int32_t>(static_cast<uint32_t>(in[0] | in[1] << 8 | in[2] << 16) | static_cast<uint32_t>(in[3]) << 24);
        in += 4;
    }
    const int rowsN = static_cast<int>(rows().size());
    if (v[2] < 0 || v[2] >= rowsN || v[3] < 0 || v[3] >= static_cast<int>(rows()[static_cast<size_t>(v[2])].size()) ||
        v[4] < -1 || v[4] >= kC64KeyCount)
        return;   // not a state of this keyboard: left as it is
    open_ = v[0]; top_ = v[1]; sel_ = {v[2], v[3]}; held_ = v[4]; heldFrames_ = v[5]; pressWas_ = v[6]; lockWas_ = v[7];
    moveWas_ = v[8]; repeatDir_ = v[9]; repeatFrames_ = v[10]; shiftLock_ = v[11]; space_ = v[12]; ret_ = v[13];
    for (bool& l : latched_) l = *in++ != 0;
}

void Osk::setFont(const uint8_t* chargen) {
    haveFont_ = chargen != nullptr;
    if (chargen) std::memcpy(font_.data(), chargen, font_.size());
}

void Osk::setOpen(bool open) {
    open_ = open;
    held_ = -1;
    pressWas_ = lockWas_ = moveWas_ = true;   // a button still down from opening does nothing
    repeatDir_ = -1;
    space_ = ret_ = false;
    if (!open) latched_.fill(false);
}

int Osk::keyAt(int row, int index) const {
    const KeyDef& k = rows()[static_cast<size_t>(row)][static_cast<size_t>(index)];
    return k.id ? keyIndex(k.id) : kLock;
}

void Osk::moveVertically(int dir) {
    const auto& l = layout();
    const int rowsN = static_cast<int>(l.keys.size());
    const Rect& from = l.keys[static_cast<size_t>(sel_.row)][static_cast<size_t>(sel_.index)];
    const int center = from.x + from.w / 2;
    const int row = (sel_.row + dir + rowsN) % rowsN;
    int best = 0, bestDist = 1 << 30;
    for (size_t i = 0; i < l.keys[static_cast<size_t>(row)].size(); ++i) {
        const Rect& k = l.keys[static_cast<size_t>(row)][i];
        const int d = std::abs(k.x + k.w / 2 - center);
        if (d < bestDist) { bestDist = d; best = static_cast<int>(i); }
    }
    sel_ = {row, best};
}

void Osk::update(const Pad& pad) {
    if (!open_) return;
    // The cursor: a step on the press, then after 15 frames one every 4.
    const int dir = pad.up ? 0 : pad.down ? 1 : pad.left ? 2 : pad.right ? 3 : -1;
    bool step = false;
    if (dir != repeatDir_) { repeatDir_ = dir; repeatFrames_ = 0; step = dir >= 0; }
    else if (dir >= 0 && ++repeatFrames_ >= 15 && (repeatFrames_ - 15) % 4 == 0) step = true;
    if (step) {
        const int n = static_cast<int>(rows()[static_cast<size_t>(sel_.row)].size());
        if (dir == 0) moveVertically(-1);
        else if (dir == 1) moveVertically(1);
        else if (dir == 2) sel_.index = (sel_.index + n - 1) % n;
        else sel_.index = (sel_.index + 1) % n;
    }
    if (pad.move && !moveWas_) top_ = !top_;
    moveWas_ = pad.move;
    if (pad.lock && !lockWas_) shiftLock_ = !shiftLock_;
    lockWas_ = pad.lock;
    space_ = pad.space;
    ret_ = pad.ret;

    // B: a modifier toggles its latch; another key is held while B is, at least 3 frames
    // (a press shorter than the KERNAL's scan could be missed).
    if (held_ >= 0) {
        ++heldFrames_;
        if (!pad.press && heldFrames_ >= 3) {
            held_ = -1;
            latched_.fill(false);   // the modifiers went with that key
        }
    }
    if (pad.press && !pressWas_ && held_ < 0) {
        const int k = keyAt(sel_.row, sel_.index);
        const char* id = k >= 0 ? kC64Keys[k].id : "";
        if (k == kLock) shiftLock_ = !shiftLock_;
        else if (!std::strcmp(id, "lshift") || !std::strcmp(id, "rshift") || !std::strcmp(id, "cbm") || !std::strcmp(id, "ctrl"))
            latched_[static_cast<size_t>(k)] = !latched_[static_cast<size_t>(k)];
        else if (k >= 0) { held_ = k; heldFrames_ = 0; }
    }
    pressWas_ = pad.press;
}

void Osk::keysDown(std::array<bool, kC64KeyCount>& down) const {
    if (!open_) return;
    for (int k = 0; k < kC64KeyCount; ++k)
        if (latched_[static_cast<size_t>(k)]) down[static_cast<size_t>(k)] = true;
    if (held_ >= 0) down[static_cast<size_t>(held_)] = true;
    if (shiftLock_) down[static_cast<size_t>(keyIndex("lshift"))] = true;
    if (space_) down[static_cast<size_t>(keyIndex("space"))] = true;
    if (ret_) down[static_cast<size_t>(keyIndex("return"))] = true;
}

void Osk::draw(uint32_t* picture, size_t pitch, int w, int h) const {
    if (!open_) return;
    const Layout& l = layout();
    const int px = (w - l.w) / 2;
    const int py = top_ ? 4 : h - l.h - 4;
    auto put = [&](int x, int y, uint32_t c, int parts) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint32_t& p = picture[static_cast<size_t>(y) * pitch + static_cast<size_t>(x)];
        p = parts >= 4 ? c : blend(p, c, parts, 4);
    };
    // The panel: the picture darkened under it.
    for (int y = 0; y < l.h; ++y)
        for (int x = 0; x < l.w; ++x) put(px + x, py + y, 0x101018, 3);
    const auto& r = rows();
    std::array<bool, kC64KeyCount> down{};
    keysDown(down);
    for (size_t ri = 0; ri < r.size(); ++ri) {
        for (size_t i = 0; i < r[ri].size(); ++i) {
            const Rect& k = l.keys[ri][i];
            const int key = keyAt(static_cast<int>(ri), static_cast<int>(i));
            const bool selected = static_cast<int>(ri) == sel_.row && static_cast<int>(i) == sel_.index;
            const bool isDown = key == kLock ? shiftLock_ : (key >= 0 && down[static_cast<size_t>(key)]);
            // Key faces: the C64's own grays; the cursor in its light blue; keys held down in
            // its light green.
            const uint32_t face = isDown ? 0x9AD284 : selected ? 0x7869C4 : 0x444444;
            const uint32_t edge = selected ? 0xC0B8F0 : 0x6C6C6C;
            for (int y = 0; y < kKeyH; ++y)
                for (int x = 0; x < k.w; ++x) {
                    const bool border = y == 0 || x == 0 || y == kKeyH - 1 || x == k.w - 1;
                    const bool corner = (y == 0 || y == kKeyH - 1) && (x == 0 || x == k.w - 1);
                    if (!corner) put(px + k.x + x, py + k.y + y, border ? edge : face, 4);
                }
            const char* label = r[ri][i].label;
            const int n = static_cast<int>(std::strlen(label));
            const int tx = px + k.x + (k.w - 8 * n) / 2, ty = py + k.y + (kKeyH - 8) / 2 + 1;
            const uint32_t ink = isDown ? 0x202020 : 0xEEEEEE;
            for (int c = 0; c < n; ++c) {
                const uint8_t ch = static_cast<uint8_t>(label[c]);
                const uint8_t* glyph = nullptr;
                if (ch == 0x01) glyph = kDownArrow;
                else if (ch == 0x02) glyph = kRightArrow;
                else if (haveFont_) {
                    const unsigned code = ch >= 'A' && ch <= 'Z' ? ch - 64u : ch == '@' ? 0u : ch;
                    glyph = &font_[(code & 0x7F) * 8u];
                }
                if (!glyph) continue;
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        if (glyph[y] & (0x80 >> x)) put(tx + 8 * c + x, ty + y, ink, 4);
            }
        }
    }
}

} // namespace k64lr
