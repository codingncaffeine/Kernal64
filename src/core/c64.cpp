#include "c64.h"

#include <algorithm>
#include <cstring>

namespace k64 {

namespace {
constexpr uint32_t kStateMagic = 0x3436'4B32; // "K64" + format generation
constexpr uint32_t kStateVersion = 1;
}

C64::C64(VicModel model) : timing_(&timingFor(model)), raster_(kMaxRasterWidth * kMaxRasterHeight, 0) {
    reset();
}

void C64::setModel(VicModel model) {
    timing_ = &timingFor(model);
    reset();
}

void C64::reset() {
    cycle_ = 0;
    frame_ = 0;
    rasterX_ = 0;
    rasterY_ = 0;
    std::fill(raster_.begin(), raster_.end(), 0u);
}

void C64::tick() {
    ++cycle_;
    if (++rasterX_ == timing_->cyclesPerLine) {
        rasterX_ = 0;
        if (++rasterY_ == timing_->linesPerFrame) {
            rasterY_ = 0;
            ++frame_;
        }
    }
}

void C64::runFrame() {
    const uint64_t start = frame_;
    while (frame_ == start) tick();
}

size_t C64::stateSize() const {
    return 4 + 4 + 1 + 8 + 8 + 4 + 4;
}

void C64::saveState(uint8_t* out) const {
    auto put = [&out](const void* p, size_t n) { std::memcpy(out, p, n); out += n; };
    put(&kStateMagic, 4);
    put(&kStateVersion, 4);
    const uint8_t model = static_cast<uint8_t>(timing_->model);
    put(&model, 1);
    put(&cycle_, 8);
    put(&frame_, 8);
    const int32_t x = rasterX_, y = rasterY_;
    put(&x, 4);
    put(&y, 4);
}

bool C64::loadState(const uint8_t* in, size_t size) {
    if (size < stateSize()) return false;
    auto get = [&in](void* p, size_t n) { std::memcpy(p, in, n); in += n; };
    uint32_t magic = 0, version = 0;
    uint8_t model = 0;
    get(&magic, 4);
    get(&version, 4);
    get(&model, 1);
    if (magic != kStateMagic || version != kStateVersion || model != static_cast<uint8_t>(timing_->model)) return false;
    int32_t x = 0, y = 0;
    get(&cycle_, 8);
    get(&frame_, 8);
    get(&x, 4);
    get(&y, 4);
    if (x < 0 || x >= timing_->cyclesPerLine || y < 0 || y >= timing_->linesPerFrame) return false;
    rasterX_ = x;
    rasterY_ = y;
    return true;
}

} // namespace k64
