#pragma once
// Conversion of camera raw frames into linear 16-bit RGB working images.

#include "core/Frame.h"

namespace lm {

enum class DemosaicMethod {
    Bilinear,        // fast, used for live preview
    MalvarHeCutler,  // high quality gradient-corrected linear interpolation (captures)
};

// Converts any supported RawFrame to linear RGB, scaled so that the sensor's
// full scale (2^bitDepth - 1) maps to 65535.
Image16 toLinearRGB(const RawFrame &raw, DemosaicMethod method);

} // namespace lm
