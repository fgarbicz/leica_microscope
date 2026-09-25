#include "StainAnalysis.h"

#include "core/Parallel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace lm {

namespace {

using Vec3 = std::array<double, 3>;

Vec3 normalize(const double v[3])
{
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return n > 0 ? Vec3{v[0] / n, v[1] / n, v[2] / n} : Vec3{0, 0, 0};
}

// inverse of a 3x3 matrix (rows)
bool invert(const std::array<Vec3, 3> &m, std::array<Vec3, 3> &inv)
{
    const double a = m[0][0], b = m[0][1], c = m[0][2];
    const double d = m[1][0], e = m[1][1], f = m[1][2];
    const double g = m[2][0], h = m[2][1], i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12)
        return false;
    inv[0] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det};
    inv[1] = {(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det};
    inv[2] = {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    return true;
}

double srgbToLinear(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

} // namespace

StainResult analyzeStains(const Image16 &img, const StainOptions &opt, const std::function<bool(int, int)> &inside)
{
    StainResult r;
    if (img.empty())
        return r;
    const int W = img.width, H = img.height;
    r.width = W;
    r.height = H;
    r.h.assign(size_t(W) * H, 0.f);
    r.dab.assign(size_t(W) * H, 0.f);
    r.mask.assign(size_t(W) * H, 0);

    // linearisation table (input 16 bit)
    std::vector<float> lin(65536);
    for (int v = 0; v < 65536; ++v)
        lin[v] = float(opt.displayReferred ? srgbToLinear(v / 65535.0) : v / 65535.0);

    // background (white) level per channel: 99.5th percentile of the image
    for (int c = 0; c < 3; ++c) {
        std::vector<uint32_t> hist(1024, 0);
        uint64_t n = 0;
        for (int y = 0; y < H; y += 2)
            for (int x = 0; x < W; x += 2) {
                hist[img.row(y)[x * 3 + c] >> 6]++;
                ++n;
            }
        uint64_t acc = 0;
        int k = 1023;
        for (int i = 0; i < 1024; ++i) {
            acc += hist[i];
            if (acc >= n * 0.995) {
                k = i;
                break;
            }
        }
        r.background[c] = std::max(0.05, double(lin[std::min(65535, (k << 6) + 32)]));
    }

    // stain matrix: rows = OD vectors of H, DAB and a residual (orthogonal) channel
    const Vec3 hv = normalize(opt.vectors.h), dv = normalize(opt.vectors.dab);
    Vec3 res = {hv[1] * dv[2] - hv[2] * dv[1], hv[2] * dv[0] - hv[0] * dv[2], hv[0] * dv[1] - hv[1] * dv[0]};
    const double rn = std::sqrt(res[0] * res[0] + res[1] * res[1] + res[2] * res[2]);
    for (auto &v : res)
        v /= rn > 0 ? rn : 1;
    std::array<Vec3, 3> m{hv, dv, res}, inv;
    if (!invert(m, inv))
        return r;

    // intensity class limits (DAB OD)
    const double tWeak = opt.dabThreshold, tMod = std::max(tWeak, 0.35), tStrong = std::max(tMod, 0.6);
    struct Acc {
        uint64_t region = 0, tissue = 0, pos = 0, weak = 0, mod = 0, strong = 0;
        double dabPos = 0, dabTissue = 0;
    };
    std::vector<Acc> rows(static_cast<size_t>(H));
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Acc a;
            const uint16_t *p = img.row(y);
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                if (inside && !inside(x, y))
                    continue;
                ++a.region;
                double od[3];
                for (int c = 0; c < 3; ++c) {
                    const double t = std::clamp(lin[p[x * 3 + c]] / r.background[c], 1e-3, 1.0);
                    od[c] = -std::log10(t);
                }
                // concentrations: OD = c * M  ->  c = OD * inv(M)
                const double ch = od[0] * inv[0][0] + od[1] * inv[1][0] + od[2] * inv[2][0];
                const double cd = od[0] * inv[0][1] + od[1] * inv[1][1] + od[2] * inv[2][1];
                r.h[i] = float(std::max(0.0, ch));
                r.dab[i] = float(std::max(0.0, cd));
                const double total = od[0] + od[1] + od[2];
                if (total < opt.tissueThreshold)
                    continue;
                ++a.tissue;
                r.mask[i] = 1;
                a.dabTissue += std::max(0.0, cd);
                if (cd >= tWeak) {
                    r.mask[i] = 2;
                    ++a.pos;
                    a.dabPos += cd;
                    if (cd >= tStrong)
                        ++a.strong;
                    else if (cd >= tMod)
                        ++a.mod;
                    else
                        ++a.weak;
                }
            }
            rows[size_t(y)] = a;
        }
    });
    Acc t;
    for (const auto &a : rows) {
        t.region += a.region;
        t.tissue += a.tissue;
        t.pos += a.pos;
        t.weak += a.weak;
        t.mod += a.mod;
        t.strong += a.strong;
        t.dabPos += a.dabPos;
        t.dabTissue += a.dabTissue;
    }
    r.regionPixels = t.region;
    r.tissuePixels = t.tissue;
    r.positivePixels = t.pos;
    r.meanDabPositive = t.pos ? t.dabPos / t.pos : 0.0;
    r.meanDabTissue = t.tissue ? t.dabTissue / t.tissue : 0.0;
    r.positiveFraction = t.tissue ? double(t.pos) / t.tissue : 0.0;
    if (t.tissue) {
        r.weak = double(t.weak) / t.tissue;
        r.moderate = double(t.mod) / t.tissue;
        r.strong = double(t.strong) / t.tissue;
        r.hScore = 100.0 * (1 * r.weak + 2 * r.moderate + 3 * r.strong);
    }
    const double a2 = opt.umPerPixel * opt.umPerPixel;
    r.tissueAreaUm2 = t.tissue * a2;
    r.positiveAreaUm2 = t.pos * a2;
    return r;
}

} // namespace lm
