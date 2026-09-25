#include "Debayer.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lm {

namespace {

inline uint16_t clamp16(float v)
{
    return v <= 0.f ? 0 : v >= 65535.f ? 65535 : uint16_t(v + 0.5f);
}

inline uint16_t clamp16i(int v)
{
    return v <= 0 ? 0 : v >= 65535 ? 65535 : uint16_t(v);
}

// Location of the red sample within the 2x2 Bayer cell.
void redOffset(PixelFormat f, int &rx, int &ry)
{
    switch (f) {
    case PixelFormat::BayerRG8:
    case PixelFormat::BayerRG16: rx = 0; ry = 0; break;
    case PixelFormat::BayerGR8:
    case PixelFormat::BayerGR16: rx = 1; ry = 0; break;
    case PixelFormat::BayerGB8:
    case PixelFormat::BayerGB16: rx = 0; ry = 1; break;
    default: rx = 1; ry = 1; break; // BG
    }
}

// Copies the mosaic into an int32 buffer with a 2-pixel mirrored border so the
// interpolation kernels never need bounds checks. Mirroring by 2 keeps the CFA
// phase intact.
struct Padded {
    int w = 0, h = 0, pw = 0;
    std::vector<int32_t> v;
    const int32_t *at(int x, int y) const { return v.data() + size_t(y + 2) * pw + (x + 2); }
};

Padded padMosaic(const RawFrame &raw, float scale)
{
    Padded p;
    p.w = raw.width;
    p.h = raw.height;
    p.pw = raw.width + 4;
    p.v.resize(size_t(p.pw) * (raw.height + 4));
    const bool wide = is16Bit(raw.format);
    const int maxv = (1 << raw.bitDepth) - 1;
    parallelRows(raw.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            int32_t *dst = p.v.data() + size_t(y + 2) * p.pw + 2;
            const uint8_t *src = raw.data.data() + size_t(y) * raw.stride;
            if (wide) {
                const uint16_t *s = reinterpret_cast<const uint16_t *>(src);
                for (int x = 0; x < raw.width; ++x)
                    dst[x] = int32_t(std::min<int>(s[x], maxv) * scale + 0.5f);
            } else {
                for (int x = 0; x < raw.width; ++x)
                    dst[x] = int32_t(src[x] * scale + 0.5f);
            }
        }
    });
    // mirror rows and columns (reflect about the edge sample, keeping parity)
    auto rowPtr = [&](int y) { return p.v.data() + size_t(y + 2) * p.pw; };
    for (int y = 0; y < raw.height; ++y) {
        int32_t *r = rowPtr(y);
        r[1] = raw.width > 1 ? r[2 + 1] : r[2];      // x=-1 <- x=1
        r[0] = raw.width > 2 ? r[2 + 2] : r[2];
        int e = raw.width + 2;                        // index of x=width
        r[e] = raw.width > 1 ? r[e - 2] : r[e - 1];  // x=w   <- x=w-2
        r[e + 1] = raw.width > 2 ? r[e - 3] : r[e - 1]; // x=w+1 <- x=w-3
    }
    auto copyRow = [&](int dstY, int srcY) {
        std::memcpy(rowPtr(dstY), rowPtr(srcY), size_t(p.pw) * sizeof(int32_t));
    };
    copyRow(-1, std::min(1, raw.height - 1));
    copyRow(-2, std::min(2, raw.height - 1));
    copyRow(raw.height, std::max(0, raw.height - 2));
    copyRow(raw.height + 1, std::max(0, raw.height - 3));
    return p;
}

void demosaicBilinear(const Padded &p, int rx, int ry, Image16 &out)
{
    const int pw = p.pw;
    parallelRows(p.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uint16_t *o = out.row(y);
            const bool redRow = ((y & 1) == ry);
            for (int x = 0; x < p.w; ++x, o += 3) {
                const int32_t *c = p.at(x, y);
                const int32_t N = c[-pw], S = c[pw], W = c[-1], E = c[1];
                const bool redCol = ((x & 1) == rx);
                int r, g, b;
                if (redRow && redCol) {          // R site
                    r = c[0];
                    g = (N + S + W + E + 2) >> 2;
                    b = (c[-pw - 1] + c[-pw + 1] + c[pw - 1] + c[pw + 1] + 2) >> 2;
                } else if (!redRow && !redCol) { // B site
                    b = c[0];
                    g = (N + S + W + E + 2) >> 2;
                    r = (c[-pw - 1] + c[-pw + 1] + c[pw - 1] + c[pw + 1] + 2) >> 2;
                } else if (redRow) {             // G in red row
                    g = c[0];
                    r = (W + E + 1) >> 1;
                    b = (N + S + 1) >> 1;
                } else {                         // G in blue row
                    g = c[0];
                    r = (N + S + 1) >> 1;
                    b = (W + E + 1) >> 1;
                }
                o[0] = clamp16i(r);
                o[1] = clamp16i(g);
                o[2] = clamp16i(b);
            }
        }
    });
}

// Malvar, He, Cutler: "High-quality linear interpolation for demosaicing of
// Bayer-patterned color images", ICASSP 2004.
void demosaicMHC(const Padded &p, int rx, int ry, Image16 &out)
{
    const int pw = p.pw;
    parallelRows(p.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uint16_t *o = out.row(y);
            const bool redRow = ((y & 1) == ry);
            for (int x = 0; x < p.w; ++x, o += 3) {
                const int32_t *c = p.at(x, y);
                const int C = c[0];
                const int N = c[-pw], S = c[pw], W = c[-1], E = c[1];
                const int N2 = c[-2 * pw], S2 = c[2 * pw], W2 = c[-2], E2 = c[2];
                const int D = c[-pw - 1] + c[-pw + 1] + c[pw - 1] + c[pw + 1];
                const bool redCol = ((x & 1) == rx);
                float r, g, b;
                if (redRow == redCol) {
                    // R or B site
                    const float gv = (4.f * C + 2.f * (N + S + W + E) - (N2 + S2 + W2 + E2)) / 8.f;
                    const float other = (6.f * C + 2.f * D - 1.5f * (N2 + S2 + W2 + E2)) / 8.f;
                    g = gv;
                    if (redRow) { r = float(C); b = other; }
                    else        { b = float(C); r = other; }
                } else {
                    g = float(C);
                    // value interpolated from horizontal neighbours / vertical neighbours
                    const float horiz = (5.f * C + 4.f * (W + E) - (W2 + E2) - D + 0.5f * (N2 + S2)) / 8.f;
                    const float vert  = (5.f * C + 4.f * (N + S) - (N2 + S2) - D + 0.5f * (W2 + E2)) / 8.f;
                    if (redRow) { r = horiz; b = vert; }   // G in R row: R left/right, B up/down
                    else        { r = vert; b = horiz; }   // G in B row
                }
                o[0] = clamp16(r);
                o[1] = clamp16(g);
                o[2] = clamp16(b);
            }
        }
    });
}

inline void yuvToRgb(int Y, int U, int V, uint16_t *o)
{
    // BT.601 limited range
    const int c = Y - 16, d = U - 128, e = V - 128;
    const int r = (298 * c + 409 * e + 128) >> 8;
    const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    const int b = (298 * c + 516 * d + 128) >> 8;
    o[0] = uint16_t(std::clamp(r, 0, 255) * 257);
    o[1] = uint16_t(std::clamp(g, 0, 255) * 257);
    o[2] = uint16_t(std::clamp(b, 0, 255) * 257);
}

} // namespace

Image16 toLinearRGB(const RawFrame &raw, DemosaicMethod method)
{
    if (raw.empty())
        return {};
    Image16 out(raw.width, raw.height);
    const int bd = std::clamp(raw.bitDepth, 1, 16);
    const float scale = 65535.f / float((1 << bd) - 1);

    if (isBayer(raw.format)) {
        int rx, ry;
        redOffset(raw.format, rx, ry);
        Padded p = padMosaic(raw, scale);
        if (method == DemosaicMethod::MalvarHeCutler)
            demosaicMHC(p, rx, ry, out);
        else
            demosaicBilinear(p, rx, ry, out);
        return out;
    }

    parallelRows(raw.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t *s = raw.data.data() + size_t(y) * raw.stride;
            uint16_t *o = out.row(y);
            switch (raw.format) {
            case PixelFormat::Mono8:
                for (int x = 0; x < raw.width; ++x, o += 3)
                    o[0] = o[1] = o[2] = uint16_t(s[x] * 257);
                break;
            case PixelFormat::Mono16: {
                auto *s16 = reinterpret_cast<const uint16_t *>(s);
                for (int x = 0; x < raw.width; ++x, o += 3)
                    o[0] = o[1] = o[2] = clamp16(s16[x] * scale);
                break;
            }
            case PixelFormat::RGB8:
                for (int x = 0; x < raw.width; ++x, o += 3, s += 3) {
                    o[0] = uint16_t(s[0] * 257); o[1] = uint16_t(s[1] * 257); o[2] = uint16_t(s[2] * 257);
                }
                break;
            case PixelFormat::BGR8:
                for (int x = 0; x < raw.width; ++x, o += 3, s += 3) {
                    o[0] = uint16_t(s[2] * 257); o[1] = uint16_t(s[1] * 257); o[2] = uint16_t(s[0] * 257);
                }
                break;
            case PixelFormat::BGRA8:
                for (int x = 0; x < raw.width; ++x, o += 3, s += 4) {
                    o[0] = uint16_t(s[2] * 257); o[1] = uint16_t(s[1] * 257); o[2] = uint16_t(s[0] * 257);
                }
                break;
            case PixelFormat::RGB16: {
                auto *s16 = reinterpret_cast<const uint16_t *>(s);
                for (int x = 0; x < raw.width * 3; ++x)
                    o[x] = clamp16(s16[x] * scale);
                break;
            }
            case PixelFormat::YUYV:
                for (int x = 0; x + 1 < raw.width; x += 2, s += 4, o += 6) {
                    yuvToRgb(s[0], s[1], s[3], o);
                    yuvToRgb(s[2], s[1], s[3], o + 3);
                }
                break;
            case PixelFormat::NV12: {
                const uint8_t *uv = raw.data.data() + size_t(raw.height) * raw.stride + size_t(y / 2) * raw.stride;
                for (int x = 0; x < raw.width; ++x, o += 3)
                    yuvToRgb(s[x], uv[x & ~1], uv[(x & ~1) + 1], o);
                break;
            }
            default:
                break;
            }
        }
    });
    return out;
}

} // namespace lm
