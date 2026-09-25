#pragma once
// Flat-field ("shading") correction. A reference image of an empty, evenly
// illuminated field is captured once per objective/illumination setting; its
// smooth low-frequency profile is stored per channel and divided out of every
// frame so that vignetting and uneven Koehler illumination disappear.

#include "core/Frame.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace lm {

class ShadingCorrection {
public:
    // Full-resolution per-pixel gains (interleaved RGB floats).
    struct GainMap {
        int width = 0, height = 0;
        std::vector<float> g;
        const float *row(int y) const { return g.data() + size_t(y) * width * 3; }
    };

    ShadingCorrection() = default;

    // Builds the correction from one (or the average of several) blank-field
    // images. `smoothing` is the gaussian sigma in reference-grid pixels.
    static std::shared_ptr<ShadingCorrection> fromReference(const Image16 &blank, double smoothing = 2.0);

    bool valid() const { return m_gridW > 0; }

    // Returns gains for an image of the given size (cached, thread safe).
    std::shared_ptr<const GainMap> gainsFor(int width, int height) const;

    // Maximum gain applied (reported to the user: large values mean the
    // reference was poor or the illumination is very uneven).
    double maxGain() const { return m_maxGain; }

    bool save(const std::string &path) const;
    static std::shared_ptr<ShadingCorrection> load(const std::string &path);

    int gridWidth() const { return m_gridW; }
    int gridHeight() const { return m_gridH; }
    int sourceWidth() const { return m_srcW; }
    int sourceHeight() const { return m_srcH; }

private:
    int m_gridW = 0, m_gridH = 0;   // low resolution gain grid
    int m_srcW = 0, m_srcH = 0;     // size of the reference image
    std::vector<float> m_grid;      // interleaved RGB gains
    double m_maxGain = 1.0;

    mutable std::mutex m_cacheMutex;
    mutable std::shared_ptr<const GainMap> m_cache;
};

} // namespace lm
