#include "Registration.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cmath>

namespace lm {

namespace {
constexpr float kPi = 3.14159265358979f;

int nextPow2(int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}
} // namespace

void fft1d(std::complex<float> *a, int n, bool inverse, int stride)
{
    // bit reversal permutation
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[size_t(i) * stride], a[size_t(j) * stride]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = 2 * kPi / len * (inverse ? 1.f : -1.f);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (int j = 0; j < len / 2; ++j) {
                auto &u = a[size_t(i + j) * stride];
                auto &v = a[size_t(i + j + len / 2) * stride];
                const std::complex<float> t = v * w;
                v = u - t;
                u = u + t;
                w *= wl;
            }
        }
    }
    if (inverse) {
        const float s = 1.f / n;
        for (int i = 0; i < n; ++i)
            a[size_t(i) * stride] *= s;
    }
}

void fft2d(std::vector<std::complex<float>> &d, int w, int h, bool inverse)
{
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            fft1d(d.data() + size_t(y) * w, w, inverse, 1);
    }, 4);
    // columns: copy into contiguous buffers for cache efficiency
    parallelRows(w, [&](int x0, int x1) {
        std::vector<std::complex<float>> col(h);
        for (int x = x0; x < x1; ++x) {
            for (int y = 0; y < h; ++y)
                col[y] = d[size_t(y) * w + x];
            fft1d(col.data(), h, inverse, 1);
            for (int y = 0; y < h; ++y)
                d[size_t(y) * w + x] = col[y];
        }
    }, 4);
}

ImageF toGray(const Image16 &img, int f)
{
    f = std::max(1, f);
    ImageF out(img.width / f, img.height / f);
    parallelRows(out.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < out.width; ++x) {
                float acc = 0;
                for (int dy = 0; dy < f; ++dy) {
                    const uint16_t *p = img.row(y * f + dy) + size_t(x) * f * 3;
                    for (int dx = 0; dx < f; ++dx)
                        acc += 0.25f * p[dx * 3] + 0.5f * p[dx * 3 + 1] + 0.25f * p[dx * 3 + 2];
                }
                out.at(x, y) = acc / (f * f * 65535.f);
            }
        }
    });
    return out;
}

Shift phaseCorrelate(const ImageF &ref, const ImageF &mov)
{
    Shift res;
    const int w = std::min(ref.width, mov.width), h = std::min(ref.height, mov.height);
    if (w < 8 || h < 8)
        return res;
    const int W = nextPow2(w), H = nextPow2(h);
    std::vector<std::complex<float>> A(size_t(W) * H), B(size_t(W) * H);

    // mean-subtracted, Hann-windowed copies
    auto meanOf = [&](const ImageF &im) {
        double s = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                s += im.at(x, y);
        return float(s / (double(w) * h));
    };
    const float ma = meanOf(ref), mb = meanOf(mov);
    std::vector<float> wx(w), wy(h);
    for (int x = 0; x < w; ++x)
        wx[x] = 0.5f - 0.5f * std::cos(2 * kPi * (x + 0.5f) / w);
    for (int y = 0; y < h; ++y)
        wy[y] = 0.5f - 0.5f * std::cos(2 * kPi * (y + 0.5f) / h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float win = wx[x] * wy[y];
            A[size_t(y) * W + x] = (ref.at(x, y) - ma) * win;
            B[size_t(y) * W + x] = (mov.at(x, y) - mb) * win;
        }
    fft2d(A, W, H, false);
    fft2d(B, W, H, false);
    for (size_t i = 0; i < A.size(); ++i) {
        std::complex<float> c = A[i] * std::conj(B[i]);
        float m = std::abs(c);
        A[i] = m > 1e-12f ? c / m : std::complex<float>(0, 0);
    }
    fft2d(A, W, H, true);

    int bx = 0, by = 0;
    float best = -1e30f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float v = A[size_t(y) * W + x].real();
            if (v > best) {
                best = v;
                bx = x;
                by = y;
            }
        }
    auto val = [&](int x, int y) { return A[size_t((y + H) % H) * W + (x + W) % W].real(); };
    // parabolic sub-pixel refinement
    auto refine = [](float l, float c, float r) {
        float d = l - 2 * c + r;
        return std::abs(d) > 1e-12f ? 0.5f * (l - r) / d : 0.f;
    };
    double sx = bx + refine(val(bx - 1, by), best, val(bx + 1, by));
    double sy = by + refine(val(bx, by - 1), best, val(bx, by + 1));
    if (sx > W / 2)
        sx -= W;
    if (sy > H / 2)
        sy -= H;
    res.dx = sx;
    res.dy = sy;
    res.confidence = std::clamp(double(best), 0.0, 1.0);
    return res;
}

} // namespace lm
