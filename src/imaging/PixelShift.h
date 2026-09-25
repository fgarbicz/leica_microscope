#pragma once
// Reconstruction of sensor-shift ("pixel shift") captures. Every shot is the
// raw Bayer mosaic taken with the sensor displaced by a known sub-pixel offset.
// Shots that differ by whole pixels give every output position a true R, G
// and B sample (no demosaicing); fractional offsets increase the sampling
// density by `upscale` in each direction.

#include "core/Frame.h"

#include <utility>
#include <vector>

namespace lm {

struct PixelShiftOptions {
    int upscale = 1;         // 1 (4-shot), 2 (16-shot), 3 (36-shot)
    int signX = 1, signY = 1; // direction of the sensor motion relative to the image
};

// offsets: sensor displacement per shot in pixels (same order as shots).
// Returns linear RGB scaled to 16 bit, size (W*upscale) x (H*upscale).
Image16 reconstructPixelShift(const std::vector<RawFramePtr> &shots,
                              const std::vector<std::pair<double, double>> &offsets,
                              const PixelShiftOptions &opt);

} // namespace lm
