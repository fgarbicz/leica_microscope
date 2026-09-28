#include "StainAnalysis.h"

#include "core/Parallel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

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

// linearisation table (16-bit input) and the per-channel white level: the
// 99.5th percentile of the image
void linearAndBackground(const Image16 &img, bool displayReferred, std::vector<float> &lin, double background[3])
{
    lin.resize(65536);
    for (int v = 0; v < 65536; ++v)
        lin[size_t(v)] = float(displayReferred ? srgbToLinear(v / 65535.0) : v / 65535.0);
    const int W = img.width, H = img.height;
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
            acc += hist[size_t(i)];
            if (acc >= n * 0.995) {
                k = i;
                break;
            }
        }
        background[c] = std::max(0.05, double(lin[size_t(std::min(65535, (k << 6) + 32))]));
    }
}

// eigen decomposition of a symmetric 3x3 matrix (Jacobi); columns of v are
// eigenvectors, sorted by descending eigenvalue
void eigenSymmetric3(double a[3][3], double eval[3], double v[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            v[i][j] = i == j ? 1.0 : 0.0;
    for (int sweep = 0; sweep < 50; ++sweep) {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-22)
            break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-30)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    int idx[3] = {0, 1, 2};
    std::sort(idx, idx + 3, [&](int i, int j) { return a[i][i] > a[j][j]; });
    double vs[3][3];
    for (int k = 0; k < 3; ++k) {
        eval[k] = a[idx[k]][idx[k]];
        for (int r = 0; r < 3; ++r)
            vs[r][k] = v[r][idx[k]];
    }
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            v[r][k] = vs[r][k];
}

} // namespace

bool estimateStainVectors(const Image16 &img, StainVectors &out, bool displayReferred, std::string *message)
{
    auto fail = [&](const char *m) {
        if (message)
            *message = m;
        return false;
    };
    if (img.empty())
        return fail("no image");
    std::vector<float> lin;
    double bg[3];
    linearAndBackground(img, displayReferred, lin, bg);
    // optical densities of stained pixels (every channel above the Macenko beta,
    // below saturation), subsampled to at most ~250k
    constexpr double kBeta = 0.15, kMaxOd = 2.5;
    const size_t total = size_t(img.width) * size_t(img.height);
    const int step = std::max(1, int(std::sqrt(double(total) / 250000.0)));
    std::vector<std::array<float, 3>> ods;
    ods.reserve(total / size_t(step * step) + 1);
    for (int y = 0; y < img.height; y += step) {
        const uint16_t *p = img.row(y);
        for (int x = 0; x < img.width; x += step) {
            std::array<float, 3> od;
            bool ok = true;
            for (int c = 0; c < 3 && ok; ++c) {
                const double t = std::clamp(lin[p[x * 3 + c]] / bg[c], 1e-4, 1.0);
                od[size_t(c)] = float(-std::log10(t));
                ok = od[size_t(c)] > kBeta && od[size_t(c)] < kMaxOd;
            }
            if (ok)
                ods.push_back(od);
        }
    }
    if (ods.size() < 500)
        return fail("too little stained tissue in the image");
    // plane of the two largest principal components (second moments, as in Macenko)
    double m[3][3] = {};
    for (const auto &o : ods)
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m[i][j] += double(o[size_t(i)]) * o[size_t(j)];
    double ev[3], vec[3][3];
    eigenSymmetric3(m, ev, vec);
    Vec3 e1{vec[0][0], vec[1][0], vec[2][0]}, e2{vec[0][1], vec[1][1], vec[2][1]};
    if (e1[0] + e1[1] + e1[2] < 0)
        for (auto &v : e1)
            v = -v;
    // angle of every pixel in that plane; the 1st and 99th percentiles are the stains
    std::vector<float> phi;
    phi.reserve(ods.size());
    for (const auto &o : ods) {
        const double a = o[0] * e1[0] + o[1] * e1[1] + o[2] * e1[2];
        const double b = o[0] * e2[0] + o[1] * e2[1] + o[2] * e2[2];
        phi.push_back(float(std::atan2(b, a)));
    }
    const size_t lo = phi.size() / 100, hi = phi.size() - 1 - phi.size() / 100;
    std::nth_element(phi.begin(), phi.begin() + std::ptrdiff_t(lo), phi.end());
    const double pMin = phi[lo];
    std::nth_element(phi.begin(), phi.begin() + std::ptrdiff_t(hi), phi.end());
    const double pMax = phi[hi];
    auto dir = [&](double a) {
        double v[3];
        for (int c = 0; c < 3; ++c)
            v[c] = e1[size_t(c)] * std::cos(a) + e2[size_t(c)] * std::sin(a);
        Vec3 n = normalize(v);
        if (n[0] + n[1] + n[2] < 0)
            for (auto &x : n)
                x = -x;
        return n;
    };
    Vec3 v1 = dir(pMin), v2 = dir(pMax);
    const double cosAngle = v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
    if (cosAngle > std::cos(5.0 * 3.14159265358979 / 180.0))
        return fail("only one stain found (the image needs both haematoxylin and DAB)");
    // DAB absorbs relatively more blue than red; haematoxylin the opposite
    auto ratio = [](const Vec3 &v) { return v[2] / std::max(1e-6, v[0]); };
    if (ratio(v1) > ratio(v2))
        std::swap(v1, v2);
    for (int c = 0; c < 3; ++c) {
        out.h[c] = v1[size_t(c)];
        out.dab[c] = v2[size_t(c)];
    }
    if (message)
        *message = "estimated from " + std::to_string(ods.size()) + " stained pixels";
    return true;
}

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

    std::vector<float> lin;
    linearAndBackground(img, opt.displayReferred, lin, r.background);

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
    // pass 1: concentrations and total optical density of every pixel
    std::vector<float> total(size_t(W) * size_t(H));
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint16_t *p = img.row(y);
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x);
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
                total[i] = float(od[0] + od[1] + od[2]);
            }
        }
    });
    // pass 2: classification. With denoising, a pixel is judged by the 3x3
    // median of its neighbourhood: single noisy pixels on blank glass are
    // ignored, while edges stay where they are (unlike a blur).
    auto median9 = [&](const std::vector<float> &v, int x, int y) {
        float n[9];
        int k = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            const int yy = std::clamp(y + dy, 0, H - 1);
            for (int dx = -1; dx <= 1; ++dx)
                n[k++] = v[size_t(yy) * size_t(W) + size_t(std::clamp(x + dx, 0, W - 1))];
        }
        std::nth_element(n, n + 4, n + 9);
        return double(n[4]);
    };
    std::vector<Acc> rows(static_cast<size_t>(H));
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Acc a;
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x);
                if (inside && !inside(x, y))
                    continue;
                ++a.region;
                const double tot = opt.denoise ? median9(total, x, y) : double(total[i]);
                if (tot < opt.tissueThreshold)
                    continue;
                const double cd = opt.denoise ? median9(r.dab, x, y) : double(r.dab[i]);
                ++a.tissue;
                r.mask[i] = 1;
                a.dabTissue += cd;
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
