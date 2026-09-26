#include "Hdr.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lm {

std::vector<double> hdrExposureFactors(int exposures, double stops)
{
    std::vector<double> f;
    for (int i = 0; i < std::max(1, exposures); ++i)
        f.push_back(std::pow(2.0, stops * i));
    return f;
}

RawFramePtr mergeExposures(const std::vector<RawFramePtr> &frames, double blackLevel)
{
    if (frames.empty())
        throw std::runtime_error("HDR: no frames");
    const RawFrame &f0 = *frames.front();
    for (const auto &f : frames) {
        if (!f || f->width != f0.width || f->height != f0.height || f->format != f0.format)
            throw std::runtime_error("HDR: the frames differ in size or format");
        if (f->exposureMs <= 0)
            throw std::runtime_error("HDR: frame without exposure time");
    }
    const PixelFormat outFmt = widenedTo16(f0.format);
    if (!isBayer(outFmt) && outFmt != PixelFormat::Mono16 && outFmt != PixelFormat::RGB16)
        throw std::runtime_error("HDR needs raw (Bayer, mono or RGB) frames");
    if (frames.size() == 1)
        return frames.front();

    // shortest first: it is the reference, and the fallback where every exposure saturated
    std::vector<RawFramePtr> in = frames;
    std::sort(in.begin(), in.end(), [](const RawFramePtr &a, const RawFramePtr &b) { return a->exposureMs < b->exposureMs; });
    const double ref = in.front()->exposureMs;

    const bool wide = is16Bit(f0.format); // same for all frames (checked above)
    const int spp = bytesPerPixel(f0.format) / (wide ? 2 : 1);
    const int n = f0.width * spp; // samples per row
    const float black = float(std::clamp(blackLevel, 0.0, 0.5));
    // A sample counts fully below `kFull` and not at all above `kSat`; the ramp
    // between avoids a visible seam where one exposure takes over from another.
    constexpr float kFull = 0.80f, kSat = 0.95f;

    struct Src {
        const RawFrame *f;
        float norm; // 1 / full scale (bit depths differ once frames are averaged)
        float t;    // exposure relative to ref
    };
    std::vector<Src> src;
    for (const auto &f : in)
        src.push_back({f.get(), 1.f / float((1 << std::clamp(f->bitDepth, 1, 16)) - 1), float(f->exposureMs / ref)});

    auto out = std::make_shared<RawFrame>();
    out->width = f0.width;
    out->height = f0.height;
    out->format = outFmt;
    out->bitDepth = 16;
    out->stride = n * 2;
    out->sequence = f0.sequence;
    out->timestamp = f0.timestamp;
    out->exposureMs = ref;
    out->gain = f0.gain;
    out->data.assign(size_t(out->stride) * out->height, 0);

    auto sample = [wide](const uint8_t *row, int i) {
        return wide ? float(reinterpret_cast<const uint16_t *>(row)[i]) : float(row[i]);
    };
    parallelRows(f0.height, [&](int y0, int y1) {
        std::vector<const uint8_t *> rows(src.size());
        for (int y = y0; y < y1; ++y) {
            for (size_t k = 0; k < src.size(); ++k)
                rows[k] = src[k].f->data.data() + size_t(y) * src[k].f->stride;
            auto *o = reinterpret_cast<uint16_t *>(out->data.data() + size_t(y) * out->stride);
            for (int i = 0; i < n; ++i) {
                float num = 0, den = 0;
                for (size_t k = 0; k < src.size(); ++k) {
                    const float v = sample(rows[k], i) * src[k].norm;
                    const float w = std::clamp((kSat - v) / (kSat - kFull), 0.f, 1.f) * src[k].t;
                    // least squares with weights ~ t: v - black = t * radiance (+ noise)
                    num += w * (v - black);
                    den += w * src[k].t;
                }
                // every exposure saturated: the shortest one (t = 1), clipped
                const float r = den > 0 ? num / den : sample(rows[0], i) * src[0].norm - black;
                o[i] = saturate16((black + std::max(0.f, r)) * 65535.f);
            }
        }
    });
    return out;
}

} // namespace lm
