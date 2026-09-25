#pragma once
// Nucleus detection and classification for nuclear IHC markers (Ki-67, p53,
// ER/PR, ...): counts haematoxylin (negative) and DAB (positive) stained
// nuclei and gives the labelling index (positive / all nuclei).
//
// Works on the concentration maps of analyzeStains(): nuclei are blobs of
// nuclear stain (H + DAB) of roughly known size, found as local maxima of a
// difference-of-Gaussians band-pass tuned to the nucleus radius.

#include "imaging/StainAnalysis.h"

#include <functional>
#include <vector>

namespace lm {

struct NucleusOptions {
    double diameterUm = 7.0;     // typical nucleus diameter
    double umPerPixel = 0.0;     // 0 = uncalibrated: diameterPx is used
    double diameterPx = 28.0;
    double minContrast = 0.03;   // minimum band-pass response (OD) of a nucleus
    double minStain = 0.06;      // minimum smoothed nuclear stain (OD) at the centre
    double dabThreshold = 0.15;  // mean DAB (OD) for "positive"
    // true: nuclear marker (Ki-67, p53, ER/PR): nuclei are H or DAB stained, a nucleus is
    // positive if its own DAB exceeds the threshold. false: cytoplasmic / membranous marker:
    // nuclei are found from haematoxylin only, a cell is positive if the DAB in a ring
    // around its nucleus (its cytoplasm) exceeds the threshold.
    bool nuclearMarker = true;
};

struct Nucleus {
    float x = 0, y = 0;          // centre in image pixels
    float dab = 0, h = 0;        // mean DAB (nucleus or surrounding ring) and H (nucleus)
    bool positive = false;
};

struct NucleusResult {
    std::vector<Nucleus> nuclei;
    int positive = 0, negative = 0;
    double labellingIndex = 0.0; // positive / (positive + negative), 0..1
    double areaMm2 = 0.0;        // analysed area (region), if calibrated
    double densityPerMm2 = 0.0;
    double radiusPx = 0.0;       // nucleus radius used, for drawing
};

// inside(x, y) restricts the counting to a region (null = whole image).
NucleusResult detectNuclei(const StainResult &stains, const NucleusOptions &opt,
                           const std::function<bool(int, int)> &inside = {});

} // namespace lm
