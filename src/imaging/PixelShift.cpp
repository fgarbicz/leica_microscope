#include "PixelShift.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cmath>

namespace lm {

namespace {
// colour (0 R, 1 G, 2 B) of mosaic position (x, y)
inline int cfaColor(PixelFormat f, int x, int y)
{
    int rx, ry;
    switch (f) {
    case PixelFormat::BayerRG8: case PixelFormat::BayerRG16: rx = 0; ry = 0; break;
    case PixelFormat::BayerGR8: case PixelFormat::BayerGR16: rx = 1; ry = 0; break;
    case PixelFormat::BayerGB8: case PixelFormat::BayerGB16: rx = 0; ry = 1; break;
    default: rx = 1; ry = 1; break;
    }
    const bool redRow = (y & 1) == ry, redCol = (x & 1) == rx;
    if (redRow && redCol) return 0;
    if (!redRow && !redCol) return 2;
    return 1;
}
} // namespace

Image16 reconstructPixelShift(const std::vector<RawFramePtr> &shots,
                              const std::vector<std::pair<double, double>> &offsets,
                              const PixelShiftOptions &opt)
{
    if (shots.empty() || shots.size() != offsets.size() || !shots[0])
        return {};
    const int W = shots[0]->width, H = shots[0]->height, f = std::max(1, opt.upscale);
    const int OW = W * f, OH = H * f;
    const size_t n = size_t(OW) * OH;
    std::vector<uint32_t> sum(n * 3, 0);
    std::vector<uint8_t> cnt(n * 3, 0);

    // offsets in output units, normalised so all are >= 0
    std::vector<std::pair<int, int>> so;
    int minX = 0, minY = 0;
    for (auto &o : offsets) {
        const int sx = int(std::lround(o.first * f)) * opt.signX;
        const int sy = int(std::lround(o.second * f)) * opt.signY;
        so.push_back({sx, sy});
        minX = std::min(minX, sx);
        minY = std::min(minY, sy);
    }

    for (size_t k = 0; k < shots.size(); ++k) {
        const RawFrame &r = *shots[k];
        if (r.width != W || r.height != H || !isBayer(r.format))
            continue;
        const bool wide = is16Bit(r.format);
        const int sx = so[k].first - minX, sy = so[k].second - minY;
        parallelRows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const int oy = y * f + sy;
                if (oy < 0 || oy >= OH)
                    continue;
                const uint8_t *row = r.data.data() + size_t(y) * r.stride;
                for (int x = 0; x < W; ++x) {
                    const int ox = x * f + sx;
                    if (ox < 0 || ox >= OW)
                        continue;
                    const int c = cfaColor(r.format, x, y);
                    const uint32_t v = wide ? reinterpret_cast<const uint16_t *>(row)[x] : row[x];
                    const size_t i = (size_t(oy) * OW + ox) * 3 + c;
                    sum[i] += v;
                    if (cnt[i] < 255)
                        cnt[i]++;
                }
            }
        });
    }

    const int bd = std::clamp(shots[0]->bitDepth, 1, 16);
    const float scale = 65535.f / float((1 << bd) - 1);
    Image16 out(OW, OH);
    std::vector<uint8_t> missing(n * 3, 0);
    parallelRows(OH, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < OW; ++x)
                for (int c = 0; c < 3; ++c) {
                    const size_t i = (size_t(y) * OW + x) * 3 + c;
                    if (cnt[i]) {
                        out.px[i] = uint16_t(std::min(65535.f, float(sum[i]) / cnt[i] * scale + 0.5f));
                    } else {
                        missing[i] = 1;
                    }
                }
    });
    // fill gaps (image borders, dropped shots) from the nearest valid neighbours
    for (int pass = 0; pass < 8; ++pass) {
        bool any = false;
        for (int y = 0; y < OH; ++y)
            for (int x = 0; x < OW; ++x)
                for (int c = 0; c < 3; ++c) {
                    const size_t i = (size_t(y) * OW + x) * 3 + c;
                    if (!missing[i])
                        continue;
                    uint32_t s = 0, m = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= OW || yy >= OH)
                                continue;
                            const size_t j = (size_t(yy) * OW + xx) * 3 + c;
                            if (!missing[j]) {
                                s += out.px[j];
                                ++m;
                            }
                        }
                    if (m) {
                        out.px[i] = uint16_t(s / m);
                        missing[i] = 2; // becomes valid after this pass
                    }
                    any = true;
                }
        for (auto &v : missing)
            if (v == 2)
                v = 0;
        if (!any)
            break;
    }
    return out;
}

} // namespace lm
