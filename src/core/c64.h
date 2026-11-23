// The whole machine, advanced one system cycle at a time.
#pragma once

#include "timing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace k64 {

class C64 {
public:
    explicit C64(VicModel model = VicModel::Mos6569);

    void reset();
    void setModel(VicModel model);
    const Timing& timing() const { return *timing_; }

    // Runs exactly one video frame worth of cycles.
    void runFrame();

    // The raster as emitted, one 32-bit XRGB pixel per dot, kMaxRasterWidth wide.
    const uint32_t* raster() const { return raster_.data(); }
    int rasterWidth() const { return timing_->cyclesPerLine * 8; }
    int rasterHeight() const { return timing_->linesPerFrame; }

    uint64_t cycle() const { return cycle_; }
    uint64_t frame() const { return frame_; }

    // Machine state as a flat byte image of constant size for the model.
    size_t stateSize() const;
    void saveState(uint8_t* out) const;
    bool loadState(const uint8_t* in, size_t size);

private:
    void tick();

    const Timing* timing_;
    uint64_t cycle_ = 0;
    uint64_t frame_ = 0;
    int rasterX_ = 0;   // cycle within the line, 0-based
    int rasterY_ = 0;
    std::vector<uint32_t> raster_;
};

} // namespace k64
