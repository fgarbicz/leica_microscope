#include "Analysis.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cmath>

namespace lm {

namespace {

template <typename Img, int Shift>
Histogram histImpl(const Img &img, int step)
{
    Histogram h;
    if (img.empty())
        return h;
    step = std::max(1, step);
    uint64_t sum[4] = {0, 0, 0, 0};
    uint64_t hi = 0, lo = 0, n = 0;
    for (int c = 0; c < 4; ++c) {
        h.minV[c] = 255;
        h.maxV[c] = 0;
    }
    for (int y = 0; y < img.height; y += step) {
        const auto *p = img.row(y);
        for (int x = 0; x < img.width; x += step) {
            const auto *q = p + size_t(x) * 3;
            int v[4];
            v[0] = q[0] >> Shift;
            v[1] = q[1] >> Shift;
            v[2] = q[2] >> Shift;
            v[3] = (v[0] * 54 + v[1] * 183 + v[2] * 19) >> 8;
            for (int c = 0; c < 4; ++c) {
                h.bins[c][v[c]]++;
                sum[c] += v[c];
                h.minV[c] = std::min(h.minV[c], v[c]);
                h.maxV[c] = std::max(h.maxV[c], v[c]);
            }
            if (v[0] >= 254 || v[1] >= 254 || v[2] >= 254)
                ++hi;
            if (v[0] <= 1 && v[1] <= 1 && v[2] <= 1)
                ++lo;
            ++n;
        }
    }
    h.count = n;
    for (int c = 0; c < 4; ++c)
        h.mean[c] = n ? double(sum[c]) / n : 0.0;
    h.clippedHigh = n ? double(hi) / n : 0.0;
    h.clippedLow = n ? double(lo) / n : 0.0;
    return h;
}

} // namespace

Histogram computeHistogram(const Image8 &img, int step)
{
    return histImpl<Image8, 0>(img, step);
}

Histogram computeHistogram(const Image16 &img, int step)
{
    return histImpl<Image16, 8>(img, step);
}

Histogram computeHistogram32(const uint32_t *px, int width, int height, int stride, int step)
{
    Histogram h;
    if (!px || width <= 0 || height <= 0)
        return h;
    step = std::max(1, step);
    uint64_t sum[4] = {0, 0, 0, 0};
    uint64_t hi = 0, lo = 0, n = 0;
    for (int c = 0; c < 4; ++c) {
        h.minV[c] = 255;
        h.maxV[c] = 0;
    }
    for (int y = 0; y < height; y += step) {
        const uint32_t *row = px + size_t(y) * stride;
        for (int x = 0; x < width; x += step) {
            const uint32_t p = row[x];
            int v[4] = {int((p >> 16) & 0xFF), int((p >> 8) & 0xFF), int(p & 0xFF), 0};
            v[3] = (v[0] * 54 + v[1] * 183 + v[2] * 19) >> 8;
            for (int c = 0; c < 4; ++c) {
                h.bins[c][v[c]]++;
                sum[c] += v[c];
                h.minV[c] = std::min(h.minV[c], v[c]);
                h.maxV[c] = std::max(h.maxV[c], v[c]);
            }
            if (v[0] >= 254 || v[1] >= 254 || v[2] >= 254)
                ++hi;
            if (v[0] <= 1 && v[1] <= 1 && v[2] <= 1)
                ++lo;
            ++n;
        }
    }
    h.count = n;
    for (int c = 0; c < 4; ++c)
        h.mean[c] = n ? double(sum[c]) / n : 0.0;
    h.clippedHigh = n ? double(hi) / n : 0.0;
    h.clippedLow = n ? double(lo) / n : 0.0;
    return h;
}

double focusMeasureRaw(const RawFrame &raw, Rect r)
{
    if (raw.width < 16 || raw.height < 16)
        return 0.0;
    if (r.empty())
        r = {raw.width / 4, raw.height / 4, raw.width / 2, raw.height / 2};
    r.x = std::clamp(r.x, 2, raw.width - 3);
    r.y = std::clamp(r.y, 2, raw.height - 3);
    r.w = std::clamp(r.w, 1, raw.width - 2 - r.x);
    r.h = std::clamp(r.h, 1, raw.height - 2 - r.y);
    const bool wide = is16Bit(raw.format);
    const int bpp = bytesPerPixel(raw.format);
    auto at = [&](int x, int y) -> double {
        const uint8_t *row = raw.data.data() + size_t(y) * raw.stride;
        if (isBayer(raw.format) || isMono(raw.format))
            return wide ? reinterpret_cast<const uint16_t *>(row)[x] : row[x];
        // packed colour: use the green component
        if (raw.format == PixelFormat::RGB16)
            return reinterpret_cast<const uint16_t *>(row)[x * 3 + 1];
        return row[x * bpp + 1];
    };
    // green sites of a Bayer mosaic form a quincunx; use diagonal neighbours
    int gPhase = 0; // (x + y) parity of green sites
    if (raw.format == PixelFormat::BayerRG8 || raw.format == PixelFormat::BayerRG16 || raw.format == PixelFormat::BayerBG8
        || raw.format == PixelFormat::BayerBG16)
        gPhase = 1;
    const bool bayer = isBayer(raw.format);
    double sum = 0, sum2 = 0, mean = 0;
    uint64_t n = 0;
    const int stepY = std::max(2, r.h / 240), stepX = std::max(2, r.w / 360);
    for (int y = r.y; y < r.y + r.h; y += stepY) {
        for (int x = r.x; x < r.x + r.w; x += stepX) {
            int xx = x;
            if (bayer && ((xx + y) & 1) != gPhase)
                ++xx;
            const double c = at(xx, y);
            double lap;
            if (bayer)
                lap = 4.0 * c - at(xx - 1, y - 1) - at(xx + 1, y - 1) - at(xx - 1, y + 1) - at(xx + 1, y + 1);
            else
                lap = 4.0 * c - at(xx - 1, y) - at(xx + 1, y) - at(xx, y - 1) - at(xx, y + 1);
            sum += lap;
            sum2 += lap * lap;
            mean += c;
            ++n;
        }
    }
    if (!n)
        return 0.0;
    mean /= n;
    const double var = sum2 / n - (sum / n) * (sum / n);
    return mean > 1.0 ? var / (mean * mean) * 1000.0 : 0.0;
}

double focusMeasure(const Image16 &img, Rect r)
{
    if (img.width < 8 || img.height < 8)
        return 0.0;
    if (r.empty())
        r = {img.width / 4, img.height / 4, img.width / 2, img.height / 2};
    r.x = std::clamp(r.x, 1, img.width - 2);
    r.y = std::clamp(r.y, 1, img.height - 2);
    r.w = std::clamp(r.w, 1, img.width - 1 - r.x);
    r.h = std::clamp(r.h, 1, img.height - 1 - r.y);
    double sum = 0, sum2 = 0, mean = 0;
    uint64_t n = 0;
    const int w3 = img.width * 3;
    for (int y = r.y; y < r.y + r.h; ++y) {
        const uint16_t *p = img.row(y) + 1; // green channel
        for (int x = r.x; x < r.x + r.w; ++x) {
            const uint16_t *c = p + size_t(x) * 3;
            double lap = 4.0 * c[0] - c[-3] - c[3] - c[-w3] - c[w3];
            sum += lap;
            sum2 += lap * lap;
            mean += c[0];
            ++n;
        }
    }
    if (!n)
        return 0.0;
    mean /= n;
    double var = sum2 / n - (sum / n) * (sum / n);
    // normalise by brightness so exposure changes do not look like focus changes
    return mean > 1.0 ? var / (mean * mean) * 1000.0 : 0.0;
}

ExposureStats exposureStats(const RawFrame &raw, int step)
{
    ExposureStats s;
    if (raw.empty())
        return s;
    const bool wide = is16Bit(raw.format);
    const int bpp = bytesPerPixel(raw.format);
    const double full = wide ? double((1 << raw.bitDepth) - 1) : 255.0;
    std::vector<uint32_t> hist(1024, 0);
    uint64_t n = 0, sat = 0;
    double sum = 0;
    const int samplesPerPixel = (raw.format == PixelFormat::RGB8 || raw.format == PixelFormat::BGR8) ? 3
                                : raw.format == PixelFormat::BGRA8 ? 4
                                : raw.format == PixelFormat::RGB16 ? 3 : 1;
    const bool yuv = raw.format == PixelFormat::YUYV || raw.format == PixelFormat::NV12;
    for (int y = 0; y < raw.height; y += step) {
        const uint8_t *row = raw.data.data() + size_t(y) * raw.stride;
        for (int x = 0; x < raw.width; x += step) {
            double v;
            if (yuv) {
                v = raw.format == PixelFormat::YUYV ? row[x * 2] : row[x];
                v = std::clamp((v - 16.0) / 219.0, 0.0, 1.0);
            } else if (wide) {
                const uint16_t *p = reinterpret_cast<const uint16_t *>(row) + size_t(x) * samplesPerPixel;
                int m = p[0];
                for (int c = 1; c < std::min(samplesPerPixel, 3); ++c)
                    m = std::max<int>(m, p[c]);
                v = m / full;
            } else {
                const uint8_t *p = row + size_t(x) * bpp;
                int m = p[0];
                for (int c = 1; c < std::min(samplesPerPixel, 3); ++c)
                    m = std::max<int>(m, p[c]);
                v = m / full;
            }
            v = std::min(v, 1.0);
            hist[std::min(1023, int(v * 1023.0))]++;
            sum += v;
            if (v >= 0.985)
                ++sat;
            ++n;
        }
    }
    if (!n)
        return s;
    s.meanLevel = sum / n;
    s.saturatedFraction = double(sat) / n;
    uint64_t acc = 0;
    for (int i = 0; i < 1024; ++i) {
        acc += hist[i];
        if (acc >= n * 0.99) {
            s.percentile99 = i / 1023.0;
            break;
        }
    }
    return s;
}

ImageF localSharpness(const Image16 &img, int window)
{
    ImageF lap(img.width, img.height, 0.f);
    const int w3 = img.width * 3;
    parallelRows(img.height - 2, [&](int y0, int y1) {
        for (int yy = y0; yy < y1; ++yy) {
            const int y = yy + 1;
            const uint16_t *row = img.row(y);
            for (int x = 1; x < img.width - 1; ++x) {
                const uint16_t *c = row + size_t(x) * 3;
                // luminance-ish: sum of channels
                auto L = [&](const uint16_t *q) { return float(q[0]) + 2.f * q[1] + q[2]; };
                float cc = L(c);
                float ml = std::abs(2.f * cc - L(c - 3) - L(c + 3)) + std::abs(2.f * cc - L(c - w3) - L(c + w3));
                lap.at(x, y) = ml;
            }
        }
    });
    // box filter (integral image) to aggregate over a window
    ImageF out(img.width, img.height, 0.f);
    std::vector<double> integ(size_t(img.width + 1) * (img.height + 1), 0.0);
    const int iw = img.width + 1;
    for (int y = 0; y < img.height; ++y) {
        double rowSum = 0;
        for (int x = 0; x < img.width; ++x) {
            rowSum += lap.at(x, y);
            integ[size_t(y + 1) * iw + x + 1] = integ[size_t(y) * iw + x + 1] + rowSum;
        }
    }
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            int ya = std::max(0, y - window), yb = std::min(img.height, y + window + 1);
            for (int x = 0; x < img.width; ++x) {
                int xa = std::max(0, x - window), xb = std::min(img.width, x + window + 1);
                double s = integ[size_t(yb) * iw + xb] - integ[size_t(ya) * iw + xb] - integ[size_t(yb) * iw + xa]
                           + integ[size_t(ya) * iw + xa];
                out.at(x, y) = float(s / ((yb - ya) * (xb - xa)));
            }
        }
    });
    return out;
}

} // namespace lm
