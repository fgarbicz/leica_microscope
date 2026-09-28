#include "ColorPipeline.h"

#include "ShadingCorrection.h"
#include "core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
    // camera colour correction after white balance (it expects white = 1,1,1)
    std::array<float, 9> ccm{1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (s.colorCorrection)
        for (int i = 0; i < 9; ++i)
            ccm[size_t(i)] = float(s.cameraMatrix[size_t(i)]);
    m_matrix = mul(hm, mul(sm, mul(ccm, wb)));
    // light filter in output colour space (after white balance and colour correction)
    if (s.filterTemperature != 0.0 || s.filterTint != 0.0) {
        const auto fg = lightFilterGains(s.filterTemperature, s.filterTint);
        const std::array<float, 9> fm{float(fg[0]), 0, 0, 0, float(fg[1]), 0, 0, 0, float(fg[2])};
        m_matrix = mul(fm, m_matrix);
    }
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

std::array<double, 3> lightFilterGains(double temperature, double tint)
{
    const double t = std::clamp(temperature, -100.0, 100.0) / 100.0;
    const double n = std::clamp(tint, -100.0, 100.0) / 100.0;
    // +100 is roughly a Wratten 80A (3200 K -> 5500 K), -100 roughly an 85B
    double r = std::exp(-0.45 * t);
    double b = std::exp(0.60 * t);
    // tint: magenta = less green, green = more green (about CC30M / CC30G at +-100)
    double g = std::exp(-0.35 * n);
    const double m = std::max({r, g, b});
    return {r / m, g / m, b / m};
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
        gains = gainHolder.get(); // null: reference does not fit this image
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
                p[0] = saturate16(m[0] * r + m[1] * gg + m[2] * b);
                p[1] = saturate16(m[3] * r + m[4] * gg + m[5] * b);
                p[2] = saturate16(m[6] * r + m[7] * gg + m[8] * b);
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

void ColorPipeline::previewSize(const RawFrame &raw, int rotation, int &w, int &h, bool half)
{
    rotation = ((rotation % 360) + 360) % 360;
    const bool swap = rotation == 90 || rotation == 270;
    const int rw = half ? raw.width / 2 : raw.width, rh = half ? raw.height / 2 : raw.height;
    w = swap ? rh : rw;
    h = swap ? rw : rh;
}

void ColorPipeline::renderPreviewHalf32(const RawFrame &raw, uint32_t *out, int stride, bool showClipping) const
{
    int rx = -1, ry = -1;
    switch (raw.format) {
    case PixelFormat::BayerRG8: case PixelFormat::BayerRG16: rx = 0; ry = 0; break;
    case PixelFormat::BayerGR8: case PixelFormat::BayerGR16: rx = 1; ry = 0; break;
    case PixelFormat::BayerGB8: case PixelFormat::BayerGB16: rx = 0; ry = 1; break;
    case PixelFormat::BayerBG8: case PixelFormat::BayerBG16: rx = 1; ry = 1; break;
    default: break;
    }
    if (rx < 0 || raw.width < 2 || raw.height < 2) {
        renderPreview32(raw, out, stride, showClipping);
        return;
    }
    const int W = raw.width / 2, H = raw.height / 2;
    const int rot = ((m_settings.rotation % 360) + 360) % 360;
    const bool flipH = m_settings.flipHorizontal, flipV = m_settings.flipVertical;
    const bool wide = is16Bit(raw.format);
    const float scaleIn = 65535.f / float((1 << std::clamp(raw.bitDepth, 1, 16)) - 1);
    const float black = float(m_black), bscale = m_blackScale;
    const auto &m = m_matrix;
    const uint8_t *lut = m_lut8.data();
    std::shared_ptr<const ShadingCorrection::GainMap> gains;
    if (m_shading && m_shading->valid())
        gains = m_shading->gainsFor(W, H);
    // offsets of R, G1, G2, B inside the 2x2 cell
    const int bx = 1 - rx, by = 1 - ry;
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t *row[2] = {raw.data.data() + size_t(2 * y) * raw.stride,
                                     raw.data.data() + size_t(2 * y + 1) * raw.stride};
            auto at = [&](int x, int yy) -> float {
                return wide ? float(reinterpret_cast<const uint16_t *>(row[yy])[x]) : float(row[yy][x]);
            };
            const float *g = gains ? gains->row(y) : nullptr;
            // destination row mapping for the simple (no rotation) case
            for (int x = 0; x < W; ++x) {
                float r = at(2 * x + rx, ry) * scaleIn;
                float b = at(2 * x + bx, by) * scaleIn;
                float gg = 0.5f * (at(2 * x + bx, ry) + at(2 * x + rx, by)) * scaleIn;
                const bool clipHi = showClipping && (r >= 65000.f || gg >= 65000.f || b >= 65000.f);
                r = std::max(0.f, r - black) * bscale;
                gg = std::max(0.f, gg - black) * bscale;
                b = std::max(0.f, b - black) * bscale;
                if (g) {
                    r *= g[x * 3];
                    gg *= g[x * 3 + 1];
                    b *= g[x * 3 + 2];
                }
                const float R = m[0] * r + m[1] * gg + m[2] * b;
                const float G = m[3] * r + m[4] * gg + m[5] * b;
                const float B = m[6] * r + m[7] * gg + m[8] * b;
                uint32_t px;
                if (clipHi)
                    px = 0xFFFF0000u;
                else if (showClipping && R < 200.f && G < 200.f && B < 200.f)
                    px = 0xFF0040FFu;
                else
                    px = 0xFF000000u | uint32_t(lut[saturate16(R)]) << 16 | uint32_t(lut[saturate16(G)]) << 8 | lut[saturate16(B)];
                const int sx = flipH ? W - 1 - x : x, sy = flipV ? H - 1 - y : y;
                int ox, oy;
                switch (rot) {
                case 90: ox = H - 1 - sy; oy = sx; break;
                case 180: ox = W - 1 - sx; oy = H - 1 - sy; break;
                case 270: ox = sy; oy = W - 1 - sx; break;
                default: ox = sx; oy = sy; break;
                }
                out[size_t(oy) * stride + ox] = px;
            }
        }
    });
}

void ColorPipeline::renderPreview32(const RawFrame &raw, uint32_t *out, int stride, bool showClipping) const
{
    if (raw.empty())
        return;
    const int W = raw.width, H = raw.height;
    const int rot = ((m_settings.rotation % 360) + 360) % 360;
    const bool flipH = m_settings.flipHorizontal, flipV = m_settings.flipVertical;
    const bool wide = is16Bit(raw.format);
    const int bd = std::clamp(raw.bitDepth, 1, 16);
    const float scaleIn = 65535.f / float((1 << bd) - 1);
    const float black = float(m_black), bscale = m_blackScale;
    const auto &m = m_matrix;
    const uint8_t *lut = m_lut8.data();
    std::shared_ptr<const ShadingCorrection::GainMap> gains;
    if (m_shading && m_shading->valid())
        gains = m_shading->gainsFor(W, H);

    // Bayer layout: position of red in the 2x2 cell (or -1 for non-Bayer)
    int rx = -1, ry = -1;
    switch (raw.format) {
    case PixelFormat::BayerRG8: case PixelFormat::BayerRG16: rx = 0; ry = 0; break;
    case PixelFormat::BayerGR8: case PixelFormat::BayerGR16: rx = 1; ry = 0; break;
    case PixelFormat::BayerGB8: case PixelFormat::BayerGB16: rx = 0; ry = 1; break;
    case PixelFormat::BayerBG8: case PixelFormat::BayerBG16: rx = 1; ry = 1; break;
    default: break;
    }
    const bool bayer = rx >= 0;

    auto sample = [&](int x, int y) -> float {
        x = x < 0 ? -x : (x >= W ? 2 * W - 2 - x : x);
        y = y < 0 ? -y : (y >= H ? 2 * H - 2 - y : y);
        // 1-pixel wide/high frames: the mirror lands outside, clamp
        x = std::clamp(x, 0, W - 1);
        y = std::clamp(y, 0, H - 1);
        const uint8_t *row = raw.data.data() + size_t(y) * raw.stride;
        return wide ? float(reinterpret_cast<const uint16_t *>(row)[x]) : float(row[x]);
    };

    auto dest = [&](int x, int y) -> uint32_t * {
        int sx = flipH ? W - 1 - x : x;
        int sy = flipV ? H - 1 - y : y;
        int ox, oy;
        switch (rot) {
        case 90: ox = H - 1 - sy; oy = sx; break;
        case 180: ox = W - 1 - sx; oy = H - 1 - sy; break;
        case 270: ox = sy; oy = W - 1 - sx; break;
        default: ox = sx; oy = sy; break;
        }
        return out + size_t(oy) * stride + ox;
    };

    const bool identity = !flipH && !flipV && rot == 0;
    parallelRows(H, [&](int y0, int y1) {
        // local copies: MSVC reloads captured values after every pixel store
        const float m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3], m4 = m[4], m5 = m[5], m6 = m[6], m7 = m[7], m8 = m[8];
        const float sIn = scaleIn, blk = black, bsc = bscale;
        const uint8_t *const lt = lut;
        const bool clip = showClipping;
        std::vector<uint32_t> rowBuf(static_cast<size_t>(W));
        uint32_t *const rb = rowBuf.data();
        for (int y = y0; y < y1; ++y) {
            const float *g = gains ? gains->row(y) : nullptr;
            const bool interiorRow = y > 0 && y < H - 1;
            const uint8_t *rm = raw.data.data() + size_t(interiorRow ? y - 1 : y) * raw.stride;
            const uint8_t *r0 = raw.data.data() + size_t(y) * raw.stride;
            const uint8_t *rp = raw.data.data() + size_t(interiorRow ? y + 1 : y) * raw.stride;
            const bool redRow = bayer && ((y & 1) == ry);
            for (int x = 0; x < W; ++x) {
                float r, gg, b;
                if (bayer) {
                    const bool redCol = (x & 1) == rx;
                    float C, N, S, Wv, E, D;
                    if (interiorRow && x > 0 && x < W - 1) {
                        if (wide) {
                            const uint16_t *pm = reinterpret_cast<const uint16_t *>(rm);
                            const uint16_t *p0 = reinterpret_cast<const uint16_t *>(r0);
                            const uint16_t *pp = reinterpret_cast<const uint16_t *>(rp);
                            C = p0[x]; N = pm[x]; S = pp[x]; Wv = p0[x - 1]; E = p0[x + 1];
                            D = float(pm[x - 1]) + pm[x + 1] + pp[x - 1] + pp[x + 1];
                        } else {
                            C = r0[x]; N = rm[x]; S = rp[x]; Wv = r0[x - 1]; E = r0[x + 1];
                            D = float(rm[x - 1]) + rm[x + 1] + rp[x - 1] + rp[x + 1];
                        }
                    } else {
                        C = sample(x, y); N = sample(x, y - 1); S = sample(x, y + 1);
                        Wv = sample(x - 1, y); E = sample(x + 1, y);
                        D = sample(x - 1, y - 1) + sample(x + 1, y - 1) + sample(x - 1, y + 1) + sample(x + 1, y + 1);
                    }
                    if (redRow && redCol) { r = C; gg = 0.25f * (N + S + Wv + E); b = 0.25f * D; }
                    else if (!redRow && !redCol) { b = C; gg = 0.25f * (N + S + Wv + E); r = 0.25f * D; }
                    else if (redRow) { gg = C; r = 0.5f * (Wv + E); b = 0.5f * (N + S); }
                    else { gg = C; r = 0.5f * (N + S); b = 0.5f * (Wv + E); }
                } else {
                    // packed formats
                    const uint8_t *p = r0;
                    switch (raw.format) {
                    case PixelFormat::Mono8: r = gg = b = p[x]; break;
                    case PixelFormat::Mono16: r = gg = b = reinterpret_cast<const uint16_t *>(p)[x]; break;
                    case PixelFormat::RGB8: r = p[3 * x]; gg = p[3 * x + 1]; b = p[3 * x + 2]; break;
                    case PixelFormat::BGR8: b = p[3 * x]; gg = p[3 * x + 1]; r = p[3 * x + 2]; break;
                    case PixelFormat::BGRA8: b = p[4 * x]; gg = p[4 * x + 1]; r = p[4 * x + 2]; break;
                    case PixelFormat::RGB16: {
                        const uint16_t *q = reinterpret_cast<const uint16_t *>(p) + 3 * x;
                        r = q[0]; gg = q[1]; b = q[2];
                        break;
                    }
                    default: r = gg = b = 0; break;
                    }
                }
                r *= sIn;
                gg *= sIn;
                b *= sIn;
                const bool clipHi = clip && (r >= 65000.f || gg >= 65000.f || b >= 65000.f);
                r = std::max(0.f, r - blk) * bsc;
                gg = std::max(0.f, gg - blk) * bsc;
                b = std::max(0.f, b - blk) * bsc;
                if (g) {
                    r *= g[x * 3];
                    gg *= g[x * 3 + 1];
                    b *= g[x * 3 + 2];
                }
                const float R = m0 * r + m1 * gg + m2 * b;
                const float G = m3 * r + m4 * gg + m5 * b;
                const float B = m6 * r + m7 * gg + m8 * b;
                uint32_t px;
                if (clipHi) {
                    px = 0xFFFF0000u;
                } else if (clip && R < 200.f && G < 200.f && B < 200.f) {
                    px = 0xFF0040FFu;
                } else {
                    const uint32_t lr = lt[saturate16(R)], lg = lt[saturate16(G)], lb = lt[saturate16(B)];
                    px = 0xFF000000u | (lr << 16) | (lg << 8) | lb;
                }
                rb[x] = px;
            }
            if (identity)
                std::memcpy(out + size_t(y) * stride, rb, size_t(W) * sizeof(uint32_t));
            else
                for (int x = 0; x < W; ++x)
                    *dest(x, y) = rb[x];
        }
    });
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

void unsharpMask32(uint32_t *px, int w, int h, int stride, double amount, double radius)
{
    if (amount <= 0 || w < 4 || h < 4)
        return;
    if (radius <= 1.5) {
        // single pass 3x3 unsharp mask, in place. Each chunk keeps copies of
        // the original rows it needs; the rows just outside a chunk belong to
        // its neighbours (which sharpen them concurrently), so they are copied
        // for every chunk before any processing starts.
        const int amt = int(amount * 256);
        const size_t n = static_cast<size_t>(w);
        const int wantChunks = std::clamp(workerCount() * 2, 1, std::max(1, h / 16));
        const int chunkRows = (h + wantChunks - 1) / wantChunks;
        const int chunks = (h + chunkRows - 1) / chunkRows;
        std::vector<uint32_t> edges(size_t(chunks) * 2 * n); // per chunk: row above, row below
        for (int c = 0; c < chunks; ++c) {
            const int b = c * chunkRows, e = std::min(h, b + chunkRows);
            std::copy_n(px + size_t(std::max(b - 1, 0)) * stride, w, edges.data() + size_t(2 * c) * n);
            std::copy_n(px + size_t(std::min(e, h - 1)) * stride, w, edges.data() + size_t(2 * c + 1) * n);
        }
        parallelRows(chunks, [&](int chunkBegin, int chunkEnd) {
            std::vector<uint32_t> prev(n), cur(n), next(n);
            for (int chunk = chunkBegin; chunk < chunkEnd; ++chunk) {
                const int y0 = chunk * chunkRows, y1 = std::min(h, y0 + chunkRows);
                const uint32_t *const below = edges.data() + size_t(2 * chunk + 1) * n;
                std::copy_n(edges.data() + size_t(2 * chunk) * n, w, prev.data());
                std::copy_n(px + size_t(y0) * stride, w, cur.data());
                for (int y = y0; y < y1; ++y) {
                    // rows inside the chunk are still original until written below
                    std::copy_n(y + 1 < y1 ? px + size_t(y + 1) * stride : below, w, next.data());
                    uint32_t *const out = px + size_t(y) * stride;
                    const uint32_t *const P = prev.data(), *const Cr = cur.data(), *const N = next.data();
                    const int a = amt, ww = w;
                    for (int x = 0; x < ww; ++x) {
                        const int xl = x > 0 ? x - 1 : 0, xr = x < ww - 1 ? x + 1 : ww - 1;
                        const uint32_t c0 = Cr[x];
                        uint32_t res = c0 & 0xFF000000u;
                        for (int sh = 0; sh <= 16; sh += 8) {
                            const int sum = int((P[xl] >> sh) & 0xFF) + int((P[x] >> sh) & 0xFF) + int((P[xr] >> sh) & 0xFF)
                                            + int((Cr[xl] >> sh) & 0xFF) + int((c0 >> sh) & 0xFF) + int((Cr[xr] >> sh) & 0xFF)
                                            + int((N[xl] >> sh) & 0xFF) + int((N[x] >> sh) & 0xFF) + int((N[xr] >> sh) & 0xFF);
                            const int o = int((c0 >> sh) & 0xFF) * 9;
                            const int v = std::clamp((o + ((o - sum) * a >> 8)) / 9, 0, 255);
                            res |= uint32_t(v) << sh;
                        }
                        out[x] = res;
                    }
                    std::swap(prev, cur);
                    std::swap(cur, next);
                }
            }
        }, 1);
        return;
    }
    const int r = std::clamp(int(std::lround(radius * 1.3)), 1, 10); // box ~ gaussian sigma
    // 1) horizontal box blur via prefix sums -> tmp (interleaved, value * 64)
    std::vector<uint16_t> tmp(size_t(w) * h * 3);
    parallelRows(h, [&](int y0, int y1) {
        std::vector<int> pre(size_t(w + 1) * 3, 0);
        for (int y = y0; y < y1; ++y) {
            const uint32_t *row = px + size_t(y) * stride;
            for (int x = 0; x < w; ++x) {
                const uint32_t p = row[x];
                pre[(x + 1) * 3] = pre[x * 3] + int((p >> 16) & 0xFF);
                pre[(x + 1) * 3 + 1] = pre[x * 3 + 1] + int((p >> 8) & 0xFF);
                pre[(x + 1) * 3 + 2] = pre[x * 3 + 2] + int(p & 0xFF);
            }
            uint16_t *o = tmp.data() + size_t(y) * w * 3;
            for (int x = 0; x < w; ++x) {
                const int a = std::max(0, x - r), b = std::min(w, x + r + 1);
                const int n = b - a;
                for (int c = 0; c < 3; ++c)
                    o[x * 3 + c] = uint16_t((pre[b * 3 + c] - pre[a * 3 + c]) * 64 / n);
            }
        }
    });
    // 2) vertical box blur with a sliding row window, combined with the
    //    unsharp step (row-major, cache friendly)
    const int amt = int(amount * 256);
    parallelRows(h, [&](int y0, int y1) {
        std::vector<int> col(size_t(w) * 3, 0);
        auto addRow = [&](int y, int sign) {
            const uint16_t *t = tmp.data() + size_t(std::clamp(y, 0, h - 1)) * w * 3;
            for (int i = 0; i < w * 3; ++i)
                col[i] += sign * t[i];
        };
        for (int k = -r; k <= r; ++k)
            addRow(y0 + k, 1);
        const int n = 2 * r + 1;
        for (int y = y0; y < y1; ++y) {
            uint32_t *row = px + size_t(y) * stride;
            for (int x = 0; x < w; ++x) {
                const uint32_t p = row[x];
                int v[3];
                for (int c = 0; c < 3; ++c) {
                    const int orig = int((p >> (16 - 8 * c)) & 0xFF) * 64;
                    const int blur = col[x * 3 + c] / n;
                    v[c] = std::clamp((orig + ((orig - blur) * amt >> 8)) >> 6, 0, 255);
                }
                row[x] = (p & 0xFF000000u) | uint32_t(v[0]) << 16 | uint32_t(v[1]) << 8 | uint32_t(v[2]);
            }
            addRow(y + r + 1, 1);
            addRow(y - r, -1);
        }
    }, 32);
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
    // average the brightest 10%: in bright field that is the empty background
    uint64_t total = 0;
    for (auto v : lumHist)
        total += v;
    uint64_t acc = 0;
    int thr = 0;
    for (int i = 0; i < 256; ++i) {
        acc += lumHist[i];
        if (acc >= total * 0.9) { thr = i; break; }
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
