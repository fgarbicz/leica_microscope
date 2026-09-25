#pragma once
// Colour deconvolution of bright-field IHC images (Ruifrok & Johnston 2001)
// into haematoxylin and DAB, and DAB-positive area quantification.

#include "core/Frame.h"

#include <functional>
#include <string>
#include <vector>

namespace lm {

struct StainVectors {
    // optical density per unit stain, RGB (normalised internally)
    double h[3] = {0.650, 0.704, 0.286};   // haematoxylin
    double dab[3] = {0.268, 0.570, 0.776}; // DAB
};

struct StainOptions {
    StainVectors vectors;
    double dabThreshold = 0.15;   // DAB optical density for a "positive" pixel
    double tissueThreshold = 0.10;// total OD separating tissue from background
    bool displayReferred = true;  // input is sRGB encoded (saved images)
    double umPerPixel = 0.0;
};

struct StainResult {
    int width = 0, height = 0;
    std::vector<float> h, dab;    // per-pixel concentrations (OD units)
    std::vector<uint8_t> mask;    // 0 = outside region/background, 1 = tissue, 2 = DAB positive
    uint64_t regionPixels = 0, tissuePixels = 0, positivePixels = 0;
    double meanDabPositive = 0.0; // mean DAB OD of positive pixels
    double meanDabTissue = 0.0;
    double positiveFraction = 0.0;// positive / tissue
    double tissueAreaUm2 = 0.0, positiveAreaUm2 = 0.0;
    // intensity classes of positive pixels (weak / moderate / strong), fractions of tissue
    double weak = 0, moderate = 0, strong = 0;
    double hScore = 0.0;          // 1*weak + 2*moderate + 3*strong, in % (0..300)
    double background[3] = {1, 1, 1}; // estimated white level per channel
};

// Estimates the haematoxylin and DAB OD vectors of this slide from the image
// (Macenko et al. 2009: plane of the two main principal components of the
// stained pixels' optical densities, stains at the 1st/99th angle
// percentiles). Returns false (with a reason) if the image has too little
// tissue or only one stain.
bool estimateStainVectors(const Image16 &img, StainVectors &out, bool displayReferred = true,
                          std::string *message = nullptr);

// inside(x, y) restricts the analysis to a region (null = whole image).
StainResult analyzeStains(const Image16 &img, const StainOptions &opt,
                          const std::function<bool(int, int)> &inside = {});

} // namespace lm
