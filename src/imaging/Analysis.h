#pragma once
// Image statistics used by the live view: histograms, exposure statistics and
// focus (sharpness) measures.

#include "core/Frame.h"
#include "imaging/ColorPipeline.h"

#include <array>
#include <vector>

namespace lm {

struct Histogram {
    // 256 bins per channel (R, G, B, luminance)
    std::array<std::array<uint32_t, 256>, 4> bins{};
    std::array<double, 4> mean{};
    std::array<int, 4> minV{}, maxV{};
    double clippedHigh = 0.0; // fraction of pixels with any channel saturated
    double clippedLow = 0.0;  // fraction of pixels with all channels near zero
    uint64_t count = 0;
};

// Histogram of a display image. `step` subsamples for speed.
Histogram computeHistogram(const Image8 &img, int step = 2);
// Histogram of linear data (binned to 256 by the upper 8 bits).
Histogram computeHistogram(const Image16 &img, int step = 2);

// Histogram of 0xffRRGGBB pixels (live preview buffers).
Histogram computeHistogram32(const uint32_t *px, int width, int height, int stride, int step = 3);

// Focus measure computed directly on raw data (green Bayer sites or
// luminance); region in sensor coordinates, empty = central 50%.
double focusMeasureRaw(const RawFrame &raw, Rect region = {});

// Normalized variance of the Laplacian over the green channel; higher = sharper.
// Region empty = central 50% of the image.
double focusMeasure(const Image16 &img, Rect region = {});

// Mean linear brightness (0..1) of the brightest channel in a region, used by
// auto exposure. Also returns the fraction of saturated pixels.
struct ExposureStats {
    double meanLevel = 0.0;       // 0..1
    double percentile99 = 0.0;    // 0..1
    double saturatedFraction = 0.0;
};
ExposureStats exposureStats(const RawFrame &raw, int step = 4);

// Per-pixel local contrast (sum-modified Laplacian) for focus stacking.
ImageF localSharpness(const Image16 &img, int window = 4);

} // namespace lm
