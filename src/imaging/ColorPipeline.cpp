#include "ColorPipeline.h"

#include "ShadingCorrection.h"
#include "core/Parallel.h"

#include <algorithm>
#include <cmath>

namespace lm {

namespace {

constexpr float kLumaR = 0.2126f, kLumaG = 0.7152f, kLumaB = 0.0722f;
constexpr double kPi = 3.14159265358979323846;

std::array<float, 9> mul(const std::array<float, 9> &a, const std::array<float, 9> &b)
{
    std::array<float, 9> r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j];
    return r;
}

double srgbEncode(double x)
{
    return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
}

inline uint16_t sat16(float v)
{
    return v <= 0.f ? 0 : v >= 65535.f ? 65535 : uint16_t(v + 0.5f);
}

std::vector<float> gaussianKernel(double sigma)
{
    sigma = std::max(0.3, sigma);
    int r = std::max(1, int(std::ceil(sigma * 3.0)));
    std::vector<float> k(2 * r + 1);
    double sum = 0;
    for (int i = -r; i <= r; ++i) {
        k[i + r] = float(std::exp(-(i * i) / (2.0 * sigma * sigma)));
        sum += k[i + r];
    }
    for (auto &v : k)
        v = float(v / sum);
    return k;
}

// Separable gaussian blur of an interleaved 3 channel image into float buffer.
template <typename T>
std::vector<float> blur3(const T *px, int w, int h, double sigma)
{
    const auto k = gaussianKernel(sigma);
    const int r = int(k.size() / 2);
    std::vector<float> tmp(size_t(w) * h * 3), out(size_t(w) * h * 3);
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const T *row = px + size_t(y) * w * 3;
            float *o = tmp.data() + size_t(y) * w * 3;
            for (int x = 0; x < w; ++x) {
                float a = 0, b = 0, c = 0;
                for (int i = -r; i <= r; ++i) {
                    int xx = std::clamp(x + i, 0, w - 1);
                    float kv = k[i + r];
                    a += kv * row[xx * 3];
                    b += kv * row[xx * 3 + 1];
                    c += kv * row[xx * 3 + 2];
                }
                o[x * 3] = a; o[x * 3 + 1] = b; o[x * 3 + 2] = c;
            }
        }
    });
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float *o = out.data() + size_t(y) * w * 3;
            for (int x = 0; x < w * 3; ++x) {
                float a = 0;
                for (int i = -r; i <= r; ++i) {
                    int yy = std::clamp(y + i, 0, h - 1);
                    a += k[i + r] * tmp[size_t(yy) * w * 3 + x];
                }
                o[x] = a;
            }
        }
    });
    return out;
}

template <typename Img>
Img geometryImpl(const Img &in, bool flipH, bool flipV, int rotation)
{
    rotation = ((rotation % 360) + 360) % 360;
    if (!flipH && !flipV && rotation == 0)
        return in;
    const int w = in.width, h = in.height;
    const bool swap = rotation == 90 || rotation == 270;
    Img out(swap ? h : w, swap ? w : h);
    parallelRows(out.height, [&](int y0, int y1) {
        for (int oy = y0; oy < y1; ++oy) {
            for (int ox = 0; ox < out.width; ++ox) {
                // map output coordinate back to (flipped) source coordinate
                int sx, sy;
                switch (rotation) {
                case 90:  sx = oy;          sy = h - 1 - ox; break;
                case 180: sx = w - 1 - ox;  sy = h - 1 - oy; break;
                case 270: sx = w - 1 - oy;  sy = ox;         break;
                default:  sx = ox;          sy = oy;         break;
                }
                if (flipH) sx = w - 1 - sx;
                if (flipV) sy = h - 1 - sy;
                const auto *s = in.row(sy) + sx * 3;
                auto *d = out.row(oy) + ox * 3;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            }
        }
    });
    return out;
}

template <typename Img, typename Max>
void unsharpImpl(Img &img, double amount, double radius, Max maxv)
{
    if (amount <= 0.0 || img.empty())
        return;
    auto blurred = blur3(img.px.data(), img.width, img.height, radius);
    const float a = float(amount);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto *p = img.row(y);
            const float *b = blurred.data() + size_t(y) * img.width * 3;
            for (int i = 0; i < img.width * 3; ++i) {
                float v = p[i] + a * (p[i] - b[i]);
                p[i] = v <= 0.f ? 0 : v >= float(maxv) ? maxv : decltype(maxv)(v + 0.5f);
            }
        }
    });
}

} // namespace

ColorPipeline::ColorPipeline()
{
    update(ColorSettings{});
}

void ColorPipeline::update(const ColorSettings &s)
{
    m_settings = s;
    // white balance
    std::array<float, 9> wb{float(s.wbRed), 0, 0, 0, float(s.wbGreen), 0, 0, 0, float(s.wbBlue)};
    // saturation around luma
    const float sat = float(std::clamp(s.saturation, 0.0, 4.0));
    std::array<float, 9> sm{};
    const float lw[3] = {kLumaR, kLumaG, kLumaB};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            sm[i * 3 + j] = (1.f - sat) * lw[j] + (i == j ? sat : 0.f);
    // hue rotation about the neutral axis (luma preserving approximation)
    const double a = s.hue * kPi / 180.0;
    const float c = float(std::cos(a)), sn = float(std::sin(a));
    // (W3C feHueRotate matrix)
    std::array<float, 9> hm{
        0.213f + c * 0.787f - sn * 0.213f, 0.715f - c * 0.715f - sn * 0.715f, 0.072f - c * 0.072f + sn * 0.928f,
        0.213f - c * 0.213f + sn * 0.143f, 0.715f + c * 0.285f + sn * 0.140f, 0.072f - c * 0.072f - sn * 0.283f,
        0.213f - c * 0.213f - sn * 0.787f, 0.715f - c * 0.715f + sn * 0.715f, 0.072f + c * 0.928f + sn * 0.072f,
    };
    if (s.hue == 0.0)
        hm = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    m_matrix = mul(hm, mul(sm, wb));
    if (s.grayscale) {
        // collapse to luma after white balance
        std::array<float, 9> g{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                g[i * 3 + j] = lw[j];
        m_matrix = mul(g, wb);
    }
    const double bl = std::clamp(s.blackLevel, 0.0, 0.9);
    m_black = uint16_t(bl * 65535.0 + 0.5);
    m_blackScale = float(1.0 / (1.0 - bl));
    buildTables();
}

void ColorPipeline::buildTables()
{
    const auto &s = m_settings;
    m_lut8.resize(65536);
    m_lut16.resize(65536);
    const double bp = std::clamp(s.blackPoint, 0.0, 1.0);
    const double wp = std::max(bp + 1e-4, std::clamp(s.whitePoint, 0.0, 1.0));
    const double g = std::clamp(s.gamma, 0.05, 10.0);
    for (int i = 0; i < 65536; ++i) {
        double x = i / 65535.0;
        x = std::clamp((x - bp) / (wp - bp), 0.0, 1.0);
        if (s.srgbEncode)
            x = srgbEncode(x);
        if (g != 1.0)
            x = std::pow(x, 1.0 / g);
        x = (x - 0.5) * s.contrast + 0.5 + s.brightness;
        x = std::clamp(x, 0.0, 1.0);
        if (s.invert)
            x = 1.0 - x;
        m_lut8[i] = uint8_t(x * 255.0 + 0.5);
        m_lut16[i] = uint16_t(x * 65535.0 + 0.5);
    }
}

void ColorPipeline::applyLinear(Image16 &img) const
{
    if (img.empty())
        return;
    const auto &m = m_matrix;
    const bool identity = m == std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1} && m_black == 0;
    std::shared_ptr<const ShadingCorrection> shading = m_shading;
    const ShadingCorrection::GainMap *gains = nullptr;
    std::shared_ptr<const ShadingCorrection::GainMap> gainHolder;
    if (shading && shading->valid()) {
        gainHolder = shading->gainsFor(img.width, img.height);
        gains = gainHolder.get();
    }
    if (identity && !gains)
        return;
    const float black = float(m_black), bscale = m_blackScale;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uint16_t *p = img.row(y);
            const float *g = gains ? gains->row(y) : nullptr;
            for (int x = 0; x < img.width; ++x, p += 3) {
                float r = std::max(0.f, p[0] - black) * bscale;
                float gg = std::max(0.f, p[1] - black) * bscale;
                float b = std::max(0.f, p[2] - black) * bscale;
                if (g) {
                    r *= g[x * 3];
                    gg *= g[x * 3 + 1];
                    b *= g[x * 3 + 2];
                }
                p[0] = sat16(m[0] * r + m[1] * gg + m[2] * b);
                p[1] = sat16(m[3] * r + m[4] * gg + m[5] * b);
                p[2] = sat16(m[6] * r + m[7] * gg + m[8] * b);
            }
        }
    });
}

Image8 ColorPipeline::toDisplay8(const Image16 &lin) const
{
    Image8 out(lin.width, lin.height);
    const uint8_t *lut = m_lut8.data();
    parallelRows(lin.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint16_t *s = lin.row(y);
            uint8_t *d = out.row(y);
            for (int i = 0; i < lin.width * 3; ++i)
                d[i] = lut[s[i]];
        }
    });
    return out;
}

Image16 ColorPipeline::toDisplay16(const Image16 &lin) const
{
    Image16 out(lin.width, lin.height);
    const uint16_t *lut = m_lut16.data();
    parallelRows(lin.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint16_t *s = lin.row(y);
            uint16_t *d = out.row(y);
            for (int i = 0; i < lin.width * 3; ++i)
                d[i] = lut[s[i]];
        }
    });
    return out;
}

Image8 ColorPipeline::render8(Image16 linear) const
{
    applyLinear(linear);
    linear = applyGeometry(linear, m_settings.flipHorizontal, m_settings.flipVertical, m_settings.rotation);
    Image8 out = toDisplay8(linear);
    unsharpMask(out, m_settings.sharpenAmount, m_settings.sharpenRadius);
    return out;
}

Image16 ColorPipeline::render16(Image16 linear) const
{
    applyLinear(linear);
    linear = applyGeometry(linear, m_settings.flipHorizontal, m_settings.flipVertical, m_settings.rotation);
    Image16 out = toDisplay16(linear);
    unsharpMask(out, m_settings.sharpenAmount, m_settings.sharpenRadius);
    return out;
}

Image16 applyGeometry(const Image16 &in, bool flipH, bool flipV, int rotation)
{
    return geometryImpl(in, flipH, flipV, rotation);
}

Image8 applyGeometry(const Image8 &in, bool flipH, bool flipV, int rotation)
{
    return geometryImpl(in, flipH, flipV, rotation);
}

void unsharpMask(Image16 &img, double amount, double radius)
{
    unsharpImpl(img, amount, radius, uint16_t(65535));
}

void unsharpMask(Image8 &img, double amount, double radius)
{
    unsharpImpl(img, amount, radius, uint8_t(255));
}

Image16 downscale(const Image16 &in, int f)
{
    if (f <= 1)
        return in;
    Image16 out(in.width / f, in.height / f);
    const int area = f * f;
    parallelRows(out.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uint16_t *d = out.row(y);
            for (int x = 0; x < out.width; ++x) {
                uint32_t acc[3] = {0, 0, 0};
                for (int dy = 0; dy < f; ++dy) {
                    const uint16_t *s = in.row(y * f + dy) + size_t(x) * f * 3;
                    for (int dx = 0; dx < f; ++dx) {
                        acc[0] += s[dx * 3];
                        acc[1] += s[dx * 3 + 1];
                        acc[2] += s[dx * 3 + 2];
                    }
                }
                d[x * 3] = uint16_t(acc[0] / area);
                d[x * 3 + 1] = uint16_t(acc[1] / area);
                d[x * 3 + 2] = uint16_t(acc[2] / area);
            }
        }
    });
    return out;
}

std::array<double, 3> computeWhiteBalance(const Image16 &img, Rect r)
{
    if (img.empty())
        return {1, 1, 1};
    if (r.empty())
        r = {0, 0, img.width, img.height};
    r.x = std::clamp(r.x, 0, img.width - 1);
    r.y = std::clamp(r.y, 0, img.height - 1);
    r.w = std::clamp(r.w, 1, img.width - r.x);
    r.h = std::clamp(r.h, 1, img.height - r.y);
    // Use only pixels that are neither near-black nor clipped, and weight
    // towards the brightest (background) pixels: in bright field the empty
    // background is the natural white reference.
    double sum[3] = {0, 0, 0};
    double n = 0;
    std::vector<uint32_t> lumHist(256, 0);
    for (int y = r.y; y < r.y + r.h; y += 2) {
        const uint16_t *p = img.row(y) + size_t(r.x) * 3;
        for (int x = 0; x < r.w; x += 2, p += 6) {
            int l = (p[0] + 2 * p[1] + p[2]) >> 10; // /4 then >>8
            lumHist[std::min(255, l)]++;
        }
    }
    // find the 60th percentile of luminance, average everything above it
    uint64_t total = 0;
    for (auto v : lumHist)
        total += v;
    uint64_t acc = 0;
    int thr = 0;
    for (int i = 0; i < 256; ++i) {
        acc += lumHist[i];
        if (acc >= total * 0.6) { thr = i; break; }
    }
    for (int y = r.y; y < r.y + r.h; y += 2) {
        const uint16_t *p = img.row(y) + size_t(r.x) * 3;
        for (int x = 0; x < r.w; x += 2, p += 6) {
            if (p[0] > 64000 || p[1] > 64000 || p[2] > 64000)
                continue;
            int l = (p[0] + 2 * p[1] + p[2]) >> 10;
            if (l < thr)
                continue;
            sum[0] += p[0];
            sum[1] += p[1];
            sum[2] += p[2];
            n += 1;
        }
    }
    if (n < 16 || sum[0] <= 0 || sum[1] <= 0 || sum[2] <= 0)
        return {1, 1, 1};
    // normalise so the green gain is 1 and all gains >= 1 would clip less;
    // keep green = 1 which is the usual convention.
    return {sum[1] / sum[0], 1.0, sum[1] / sum[2]};
}

double estimateBlackLevel(const Image16 &img, double percentile)
{
    if (img.empty())
        return 0.0;
    std::vector<uint32_t> hist(4096, 0);
    uint64_t n = 0;
    for (int y = 0; y < img.height; y += 2) {
        const uint16_t *p = img.row(y);
        for (int x = 0; x < img.width; x += 2, p += 6) {
            hist[std::min({p[0], p[1], p[2]}) >> 4]++;
            ++n;
        }
    }
    uint64_t acc = 0;
    for (int i = 0; i < 4096; ++i) {
        acc += hist[i];
        if (acc >= n * percentile)
            return (i << 4) / 65535.0;
    }
    return 0.0;
}

} // namespace lm
