#include "ShadingCorrection.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lm {

namespace {
// fopen with a UTF-8 path (non-ASCII user names on Windows)
FILE *openUtf8(const std::string &path, const wchar_t *wmode, const char *mode)
{
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    (void)mode;
    return _wfopen(w.c_str(), wmode);
#else
    (void)wmode;
    return std::fopen(path.c_str(), mode);
#endif
}

constexpr uint32_t kMagic = 0x4C4D5348; // "LMSH"
constexpr uint32_t kVersion = 1;

void smoothGrid(std::vector<float> &g, int w, int h, double sigma)
{
    if (sigma <= 0)
        return;
    int r = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> k(2 * r + 1);
    float sum = 0;
    for (int i = -r; i <= r; ++i)
        sum += k[i + r] = float(std::exp(-(i * i) / (2 * sigma * sigma)));
    for (auto &v : k)
        v /= sum;
    std::vector<float> tmp(g.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                float a = 0;
                for (int i = -r; i <= r; ++i)
                    a += k[i + r] * g[(size_t(y) * w + std::clamp(x + i, 0, w - 1)) * 3 + c];
                tmp[(size_t(y) * w + x) * 3 + c] = a;
            }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                float a = 0;
                for (int i = -r; i <= r; ++i)
                    a += k[i + r] * tmp[(size_t(std::clamp(y + i, 0, h - 1)) * w + x) * 3 + c];
                g[(size_t(y) * w + x) * 3 + c] = a;
            }
}
} // namespace

std::shared_ptr<ShadingCorrection> ShadingCorrection::fromReference(const Image16 &blank, double smoothing)
{
    if (blank.empty())
        return nullptr;
    auto sc = std::make_shared<ShadingCorrection>();
    const int cell = std::max(1, (std::max(blank.width, blank.height) + 159) / 160);
    const int gw = (blank.width + cell - 1) / cell;
    const int gh = (blank.height + cell - 1) / cell;
    std::vector<double> acc(size_t(gw) * gh * 3, 0.0);
    std::vector<uint32_t> cnt(size_t(gw) * gh, 0);
    for (int y = 0; y < blank.height; ++y) {
        const uint16_t *p = blank.row(y);
        const int gy = y / cell;
        for (int x = 0; x < blank.width; ++x, p += 3) {
            const size_t gi = size_t(gy) * gw + x / cell;
            acc[gi * 3] += p[0];
            acc[gi * 3 + 1] += p[1];
            acc[gi * 3 + 2] += p[2];
            cnt[gi]++;
        }
    }
    std::vector<float> grid(acc.size());
    for (size_t i = 0; i < cnt.size(); ++i)
        for (int c = 0; c < 3; ++c)
            grid[i * 3 + c] = cnt[i] ? float(acc[i * 3 + c] / cnt[i]) : 0.f;
    smoothGrid(grid, gw, gh, smoothing);

    // Normalise each channel to its maximum so the correction only brightens
    // the darker periphery (never pushes the centre into clipping); white
    // balance is handled separately, so each channel is normalised
    // independently which also removes colour casts of the illumination.
    double mx[3] = {1, 1, 1};
    for (size_t i = 0; i < cnt.size(); ++i)
        for (int c = 0; c < 3; ++c)
            mx[c] = std::max(mx[c], double(grid[i * 3 + c]));
    // use the mean of the brightest area rather than a single max pixel
    double maxGain = 1.0;
    for (size_t i = 0; i < cnt.size(); ++i)
        for (int c = 0; c < 3; ++c) {
            float v = std::max(grid[i * 3 + c], 1.f);
            float gval = float(std::clamp(mx[c] / v, 0.25, 8.0));
            grid[i * 3 + c] = gval;
            maxGain = std::max(maxGain, double(gval));
        }
    sc->m_gridW = gw;
    sc->m_gridH = gh;
    sc->m_srcW = blank.width;
    sc->m_srcH = blank.height;
    sc->m_grid = std::move(grid);
    sc->m_maxGain = maxGain;
    return sc;
}

std::shared_ptr<const ShadingCorrection::GainMap> ShadingCorrection::gainsFor(int width, int height) const
{
    // The reference must describe the same field of view: equal size or an
    // integer scale (half-resolution preview, pixel-shift upsampling). A crop
    // (e.g. a centre ROI) would stretch the vignetting profile, so it is refused.
    if (width <= 0 || height <= 0 || m_srcW <= 0 || m_srcH <= 0)
        return nullptr;
    const bool same = width == m_srcW && height == m_srcH;
    const bool down = m_srcW % width == 0 && m_srcH % height == 0 && m_srcW / width == m_srcH / height;
    const bool up = width % m_srcW == 0 && height % m_srcH == 0 && width / m_srcW == height / m_srcH;
    if (!same && !down && !up)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (m_cache && m_cache->width == width && m_cache->height == height)
        return m_cache;
    auto map = std::make_shared<GainMap>();
    map->width = width;
    map->height = height;
    map->g.resize(size_t(width) * height * 3);
    const int gw = m_gridW, gh = m_gridH;
    const float sx = float(gw) / width, sy = float(gh) / height;
    parallelRows(height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float fy = std::clamp((y + 0.5f) * sy - 0.5f, 0.f, float(gh - 1));
            int iy = std::min(int(fy), gh - 1), iy1 = std::min(iy + 1, gh - 1);
            float ty = fy - iy;
            float *o = map->g.data() + size_t(y) * width * 3;
            for (int x = 0; x < width; ++x) {
                float fx = std::clamp((x + 0.5f) * sx - 0.5f, 0.f, float(gw - 1));
                int ix = std::min(int(fx), gw - 1), ix1 = std::min(ix + 1, gw - 1);
                float tx = fx - ix;
                for (int c = 0; c < 3; ++c) {
                    float a = m_grid[(size_t(iy) * gw + ix) * 3 + c];
                    float b = m_grid[(size_t(iy) * gw + ix1) * 3 + c];
                    float d = m_grid[(size_t(iy1) * gw + ix) * 3 + c];
                    float e = m_grid[(size_t(iy1) * gw + ix1) * 3 + c];
                    o[x * 3 + c] = (a * (1 - tx) + b * tx) * (1 - ty) + (d * (1 - tx) + e * tx) * ty;
                }
            }
        }
    });
    m_cache = map;
    return map;
}

bool ShadingCorrection::save(const std::string &path) const
{
    FILE *f = openUtf8(path, L"wb", "wb");
    if (!f)
        return false;
    uint32_t hdr[6] = {kMagic, kVersion, uint32_t(m_gridW), uint32_t(m_gridH), uint32_t(m_srcW), uint32_t(m_srcH)};
    bool ok = std::fwrite(hdr, sizeof(hdr), 1, f) == 1;
    ok = ok && std::fwrite(&m_maxGain, sizeof(double), 1, f) == 1;
    ok = ok && std::fwrite(m_grid.data(), sizeof(float), m_grid.size(), f) == m_grid.size();
    std::fclose(f);
    return ok;
}

std::shared_ptr<ShadingCorrection> ShadingCorrection::load(const std::string &path)
{
    FILE *f = openUtf8(path, L"rb", "rb");
    if (!f)
        return nullptr;
    uint32_t hdr[6];
    auto sc = std::make_shared<ShadingCorrection>();
    bool ok = std::fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == kMagic && hdr[1] == kVersion
              && hdr[2] > 0 && hdr[3] > 0 && hdr[2] < 10000 && hdr[3] < 10000;
    if (ok) {
        sc->m_gridW = int(hdr[2]);
        sc->m_gridH = int(hdr[3]);
        sc->m_srcW = int(hdr[4]);
        sc->m_srcH = int(hdr[5]);
        sc->m_grid.resize(size_t(sc->m_gridW) * sc->m_gridH * 3);
        ok = std::fread(&sc->m_maxGain, sizeof(double), 1, f) == 1
             && std::fread(sc->m_grid.data(), sizeof(float), sc->m_grid.size(), f) == sc->m_grid.size();
    }
    std::fclose(f);
    return ok ? sc : nullptr;
}

} // namespace lm
