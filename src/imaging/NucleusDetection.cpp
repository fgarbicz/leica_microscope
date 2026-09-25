#include "NucleusDetection.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cmath>

namespace lm {

namespace {

// separable Gaussian blur of a float map (edges clamped)
std::vector<float> gaussian(const std::vector<float> &src, int W, int H, double sigma)
{
    const int r = std::max(1, int(std::ceil(3 * sigma)));
    std::vector<float> k(size_t(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i)
        sum += k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (auto &v : k)
        v = float(v / sum);
    std::vector<float> tmp(src.size()), out(src.size());
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float *s = src.data() + size_t(y) * size_t(W);
            float *d = tmp.data() + size_t(y) * size_t(W);
            for (int x = 0; x < W; ++x) {
                double a = 0;
                for (int i = -r; i <= r; ++i)
                    a += k[size_t(i + r)] * s[std::clamp(x + i, 0, W - 1)];
                d[x] = float(a);
            }
        }
    });
    parallelRows(H, [&](int y0, int y1) {
        std::vector<double> acc(static_cast<size_t>(W));
        for (int y = y0; y < y1; ++y) {
            std::fill(acc.begin(), acc.end(), 0.0);
            for (int i = -r; i <= r; ++i) {
                const float *s = tmp.data() + size_t(std::clamp(y + i, 0, H - 1)) * size_t(W);
                const float w = k[size_t(i + r)];
                for (int x = 0; x < W; ++x)
                    acc[size_t(x)] += w * s[x];
            }
            float *d = out.data() + size_t(y) * size_t(W);
            for (int x = 0; x < W; ++x)
                d[x] = float(acc[size_t(x)]);
        }
    });
    return out;
}

// running maximum over a (2r+1) window along rows, then columns
std::vector<float> maxFilter(const std::vector<float> &src, int W, int H, int r)
{
    std::vector<float> tmp(src.size()), out(src.size());
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float *s = src.data() + size_t(y) * size_t(W);
            float *d = tmp.data() + size_t(y) * size_t(W);
            for (int x = 0; x < W; ++x) {
                float m = s[x];
                for (int i = std::max(0, x - r), e = std::min(W - 1, x + r); i <= e; ++i)
                    m = std::max(m, s[i]);
                d[x] = m;
            }
        }
    });
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float *d = out.data() + size_t(y) * size_t(W);
            for (int x = 0; x < W; ++x)
                d[x] = tmp[size_t(y) * size_t(W) + size_t(x)];
            for (int j = std::max(0, y - r), e = std::min(H - 1, y + r); j <= e; ++j) {
                const float *s = tmp.data() + size_t(j) * size_t(W);
                for (int x = 0; x < W; ++x)
                    d[x] = std::max(d[x], s[x]);
            }
        }
    });
    return out;
}

} // namespace

NucleusResult detectNuclei(const StainResult &st, const NucleusOptions &opt, const std::function<bool(int, int)> &inside)
{
    NucleusResult res;
    const int W = st.width, H = st.height;
    if (W < 8 || H < 8 || st.h.size() != size_t(W) * size_t(H))
        return res;
    const double diamPx = opt.umPerPixel > 0 ? opt.diameterUm / opt.umPerPixel : opt.diameterPx;
    const double radius = std::max(2.0, diamPx / 2);
    res.radiusPx = radius;

    // work at reduced resolution for large nuclei (speed); the band-pass needs ~4 px per radius
    const int f = std::max(1, int(radius / 4));
    const int w = W / f, h = H / f;
    std::vector<float> stain(size_t(w) * size_t(h));
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                double a = 0;
                for (int j = 0; j < f; ++j)
                    for (int i = 0; i < f; ++i) {
                        const size_t k = size_t(y * f + j) * size_t(W) + size_t(x * f + i);
                        a += st.h[k] + (opt.nuclearMarker ? st.dab[k] : 0.f);
                    }
                stain[size_t(y) * size_t(w) + size_t(x)] = float(a / (f * f));
            }
    });
    const double r = radius / f;
    // difference of Gaussians tuned to a disk of radius r (sigma ~ r / sqrt(2))
    const double s1 = std::max(0.7, r / std::sqrt(2.0));
    const auto g1 = gaussian(stain, w, h, s1);
    const auto g2 = gaussian(stain, w, h, s1 * 1.6);
    std::vector<float> dog(g1.size());
    for (size_t i = 0; i < dog.size(); ++i)
        dog[i] = g1[i] - g2[i];
    // local maxima within ~0.7 radius (touching nuclei stay separate)
    const auto mx = maxFilter(dog, w, h, std::max(1, int(std::lround(r * 0.7))));

    std::vector<std::vector<Nucleus>> rows(static_cast<size_t>(h));
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                const float v = dog[i];
                if (v < opt.minContrast || v < mx[i] || g1[i] < opt.minStain)
                    continue;
                const float cx = float((x + 0.5) * f), cy = float((y + 0.5) * f);
                if (inside && !inside(int(cx), int(cy)))
                    continue;
                // mean haematoxylin in the inner 60 % of the nucleus; DAB there (nuclear
                // marker) or in a ring around the nucleus (cytoplasmic marker)
                const double rr = radius * 0.6;
                const double ringIn = radius * 1.05, ringOut = radius * 1.8;
                const double reach = opt.nuclearMarker ? rr : ringOut;
                double sd = 0, sh = 0;
                int nd = 0, nh = 0;
                const int step = std::max(1, int(rr / 5));
                for (int dy = -int(reach); dy <= int(reach); dy += step)
                    for (int dx = -int(reach); dx <= int(reach); dx += step) {
                        const double d2 = dx * dx + dy * dy;
                        const int px = std::clamp(int(cx) + dx, 0, W - 1), py = std::clamp(int(cy) + dy, 0, H - 1);
                        const size_t k = size_t(py) * size_t(W) + size_t(px);
                        if (d2 <= rr * rr) {
                            sh += st.h[k];
                            ++nh;
                        }
                        const bool inDab = opt.nuclearMarker ? d2 <= rr * rr
                                                             : d2 >= ringIn * ringIn && d2 <= ringOut * ringOut;
                        if (inDab) {
                            sd += st.dab[k];
                            ++nd;
                        }
                    }
                const int n = std::max(1, nd);
                Nucleus nu;
                nu.x = cx;
                nu.y = cy;
                nu.dab = float(sd / std::max(1, n));
                nu.h = float(sh / std::max(1, nh));
                nu.positive = nu.dab >= opt.dabThreshold;
                rows[size_t(y)].push_back(nu);
            }
    });
    for (auto &row : rows)
        for (auto &nu : row) {
            // plateau maxima produce neighbours with equal response: keep the first
            bool dup = false;
            for (auto it = res.nuclei.rbegin(); it != res.nuclei.rend() && it->y > nu.y - float(radius); ++it)
                if (std::hypot(it->x - nu.x, it->y - nu.y) < radius * 0.5) {
                    dup = true;
                    break;
                }
            if (!dup)
                res.nuclei.push_back(nu);
        }
    for (const auto &nu : res.nuclei)
        (nu.positive ? res.positive : res.negative)++;
    const int total = res.positive + res.negative;
    res.labellingIndex = total ? double(res.positive) / total : 0.0;
    if (opt.umPerPixel > 0) {
        const double px2 = double(st.regionPixels ? st.regionPixels : uint64_t(W) * uint64_t(H));
        res.areaMm2 = px2 * opt.umPerPixel * opt.umPerPixel * 1e-6;
        res.densityPerMm2 = res.areaMm2 > 0 ? total / res.areaMm2 : 0.0;
    }
    return res;
}

} // namespace lm
