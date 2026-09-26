#pragma once
// Translation estimation between overlapping images by phase correlation.

#include "core/Frame.h"

#include <complex>
#include <vector>

namespace lm {

// In-place radix-2 FFT of a power-of-two sized buffer.
void fft1d(std::complex<float> *data, int n, bool inverse, int stride = 1);
// 2-D FFT of a w*h row-major buffer (w and h powers of two).
void fft2d(std::vector<std::complex<float>> &data, int w, int h, bool inverse);

// Grayscale float image for registration, downscaled by `factor` (box filter).
ImageF toGray(const Image16 &img, int factor = 1);

// Bilinear resampling to any size (e.g. to put two images on one grid).
ImageF resample(const ImageF &src, int width, int height);

struct Shift {
    double dx = 0.0;         // displacement of `moving` relative to `reference`
    double dy = 0.0;         // i.e. moving(x,y) ~ reference(x+dx, y+dy)
    double confidence = 0.0; // normalised peak height (0..1), > ~0.05 is good
};

// Estimates the translation between two same-sized grayscale images.
// Both images are windowed and zero-padded to the next power of two.
Shift phaseCorrelate(const ImageF &reference, const ImageF &moving);

} // namespace lm
