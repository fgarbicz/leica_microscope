#pragma once
// Image formation pipeline applied to linear camera data:
//   black level -> shading (flat-field) -> white balance -> colour/saturation
//   -> levels (black/white point) -> contrast/brightness -> gamma -> output.

#include "core/Frame.h"

#include <array>
#include <memory>
#include <vector>

namespace lm {

class ShadingCorrection;

struct ColorSettings {
    // black level subtracted from linear data, fraction of full scale (0..0.5)
    double blackLevel = 0.0;
    // white balance gains (applied in linear space)
    double wbRed = 1.0, wbGreen = 1.0, wbBlue = 1.0;
    // 0 = monochrome, 1 = unchanged, up to 2 = strongly saturated
    double saturation = 1.0;
    // hue rotation in degrees (-180..180)
    double hue = 0.0;
    // display levels in linear output units (0..1)
    double blackPoint = 0.0;
    double whitePoint = 1.0;
    // -1..1, additive brightness after levels
    double brightness = 0.0;
    // 0..3 around mid grey, 1 = unchanged
    double contrast = 1.0;
    // output gamma exponent 1/gamma applied to normalized values; 1 = linear.
    double gamma = 1.0;
    // additionally apply sRGB transfer curve (camera data is linear)
    bool srgbEncode = true;
    // unsharp mask amount (0 = off) and radius in pixels
    double sharpenAmount = 0.0;
    double sharpenRadius = 1.0;
    // geometry
    bool flipHorizontal = false;
    bool flipVertical = false;
    int rotation = 0; // 0, 90, 180, 270
    // monochrome output
    bool grayscale = false;
    // negative / invert (useful for some contrast methods)
    bool invert = false;

    bool operator==(const ColorSettings &) const = default;
};

// Precomputed, thread-safe pipeline. Rebuild with update() whenever settings
// change; the process functions are const and may run concurrently.
class ColorPipeline {
public:
    ColorPipeline();
    void update(const ColorSettings &s);
    const ColorSettings &settings() const { return m_settings; }

    void setShading(std::shared_ptr<const ShadingCorrection> shading) { m_shading = std::move(shading); }
    std::shared_ptr<const ShadingCorrection> shading() const { return m_shading; }

    // Applies black level, shading, white balance and colour matrix in place.
    // The result is still linear ("scene referred").
    void applyLinear(Image16 &img) const;

    // Applies tone mapping to produce display referred output.
    Image8 toDisplay8(const Image16 &linear) const;
    Image16 toDisplay16(const Image16 &linear) const;

    // Convenience: full pipeline including geometry + sharpening.
    Image8 render8(Image16 linear) const;
    Image16 render16(Image16 linear) const;

private:
    void buildTables();

    ColorSettings m_settings;
    std::shared_ptr<const ShadingCorrection> m_shading;
    std::array<float, 9> m_matrix{}; // combined WB + saturation + hue matrix
    std::vector<uint8_t> m_lut8;     // 65536 entries
    std::vector<uint16_t> m_lut16;   // 65536 entries
    uint16_t m_black = 0;
    float m_blackScale = 1.f;
};

// Geometry & sharpening helpers (exposed for reuse by capture/export).
Image16 applyGeometry(const Image16 &in, bool flipH, bool flipV, int rotation);
Image8 applyGeometry(const Image8 &in, bool flipH, bool flipV, int rotation);
void unsharpMask(Image16 &img, double amount, double radius);
void unsharpMask(Image8 &img, double amount, double radius);

// Downscale by an integer factor with box filtering (fast preview path).
Image16 downscale(const Image16 &in, int factor);

// Computes white balance gains that make the mean of the given region neutral.
// Region is in image coordinates; empty region = whole image.
struct Rect { int x = 0, y = 0, w = 0, h = 0; bool empty() const { return w <= 0 || h <= 0; } };
std::array<double, 3> computeWhiteBalance(const Image16 &linear, Rect region = {});
// Estimates a black level (fraction of full scale) from the darkest percentile.
double estimateBlackLevel(const Image16 &linear, double percentile = 0.001);

} // namespace lm
