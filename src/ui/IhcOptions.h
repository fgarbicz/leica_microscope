#pragma once
// Nucleus-counting options from the user's IHC settings, shared by the
// Process page and batch quantification so both count the same way.

#include "app/AppSettings.h"
#include "imaging/NucleusDetection.h"

#include <algorithm>

namespace lm {

inline NucleusOptions nucleusOptionsFromSettings(double umPerPixel, double dabThreshold)
{
    const auto &ih = AppSettings::instance().ihc;
    NucleusOptions no;
    no.diameterUm = ih.nucleusDiameterUm;
    no.umPerPixel = umPerPixel;
    no.diameterPx = umPerPixel > 0 ? 0 : ih.nucleusDiameterUm * 4; // uncalibrated: assume 0.25 µm/px
    no.dabThreshold = dabThreshold;
    no.nuclearMarker = ih.nuclearMarker;
    static const double contrast[3] = {0.05, 0.03, 0.02}, stain[3] = {0.10, 0.06, 0.04};
    const int s = std::clamp(ih.nucleusSensitivity, 0, 2);
    no.minContrast = contrast[s];
    no.minStain = stain[s];
    return no;
}

} // namespace lm
