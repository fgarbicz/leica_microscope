#pragma once
// High dynamic range capture: raw frames of the same field at different
// exposures merged into one raw frame with more usable bits.
//
// Bright field: the background sets the exposure, so dark stains (DAB,
// haematoxylin) sit in the bottom few percent of the 12-bit range, where
// read noise dominates. Longer exposures lift them out of the noise; the merge
// keeps every sample from the exposures that did not saturate it.

#include "core/Frame.h"

#include <vector>

namespace lm {

// Exposure multipliers of an HDR capture with `exposures` frames, `stops` EV
// apart, starting at the base exposure: 3 frames, 2 stops -> 1, 4, 16.
std::vector<double> hdrExposureFactors(int exposures, double stops);

// Merges raw frames (same size and format, any exposures) into a 16-bit raw
// frame at the exposure of `reference` (0 = the shortest). Each sample is a
// least-squares fit over the unsaturated observations with weights growing with
// the exposure, which favours the long exposures where read noise dominates
// (the dark stains HDR is for); `blackLevel` (fraction of
// full scale) is removed before scaling and restored afterwards, so the result
// goes through the colour pipeline exactly like a normal capture.
RawFramePtr mergeExposures(const std::vector<RawFramePtr> &frames, double referenceExposureMs = 0,
                           double blackLevel = 0.0);

} // namespace lm
