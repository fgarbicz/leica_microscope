#include "Hdr.h"

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

namespace {
PixelFormat to16(PixelFormat f)
{
    switch (f) {
    case PixelFormat::Mono8: return PixelFormat::Mono16;
    case PixelFormat::BayerRG8: return PixelFormat::BayerRG16;
    case PixelFormat::BayerGR8: return PixelFormat::BayerGR16;
    case PixelFormat::BayerGB8: return PixelFormat::BayerGB16;
    case PixelFormat::BayerBG8: return PixelFormat::BayerBG16;
    case PixelFormat::RGB8: return PixelFormat::RGB16;
    default: return f;
    }
}
} // namespace

RawFramePtr mergeExposures(const std::vector<RawFramePtr> &frames, double referenceExposureMs, double blackLevel)
{
    std::vector<RawFramePtr> in;
    for (const auto &f : frames)
        if (f && !f->empty())
            in.push_back(f);
    if (in.empty())
        throw std::runtime_error("HDR: no frames");
    const RawFrame &f0 = *in.front();
    for (const auto &f : in) {
        if (f->width != f0.width || f->height != f0.height || f->format != f0.format)
            throw std::runtime_error("HDR: the frames differ in size or format");
        if (f->exposureMs <= 0)
            throw std::runtime_error("HDR: frame without exposure time");
    }
    const PixelFormat outFmt = to16(f0.format);
    if (!isBayer(outFmt) && outFmt != PixelFormat::Mono16 && outFmt != PixelFormat::RGB16)
        throw std::runtime_error("HDR needs raw (Bayer, mono or RGB) frames");
    if (in.size() == 1)
        return in.front();

    // shortest first: it is the fallback where every exposure saturated
    std::sort(in.begin(), in.end(), [](const RawFramePtr &a, const RawFramePtr &b) { return a->exposureMs < b->exposureMs; });
    const double ref = referenceExposureMs > 0 ? referenceExposureMs : in.front()->exposureMs;

    const int spp = bytesPerPixel(f0.format) / (is16Bit(f0.format) ? 2 : 1);
    const int n = f0.width * spp; // samples per row
    const float black = float(std::clamp(blackLevel, 0.0, 0.5));
    // A sample counts fully below `kFull` and not at all above `kSat`; the ramp
    // between avoids a visible seam where one exposure takes over from another.
    constexpr float kFull = 0.80f, kSat = 0.95f;

    struct Src {
        const RawFrame *f;
        bool wide;
        float norm; // 1 / full scale
        float t;    // exposure relative to ref
    };
    std::vector<Src> src;
    for (const auto &f : in)
        src.push_back({f.get(), is16Bit(f->format), 1.f / float((1 << std::clamp(f->bitDepth, 1, 16)) - 1),
                       float(f->exposureMs / ref)});

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

    std::vector<const uint8_t *> rows(src.size());
    for (int y = 0; y < f0.height; ++y) {
        for (size_t k = 0; k < src.size(); ++k)
            rows[k] = src[k].f->data.data() + size_t(y) * src[k].f->stride;
        auto *o = reinterpret_cast<uint16_t *>(out->data.data() + size_t(y) * out->stride);
        for (int i = 0; i < n; ++i) {
            float num = 0, den = 0;
            float fallback = -1;
            for (size_t k = 0; k < src.size(); ++k) {
                const Src &s = src[k];
                const float v = (s.wide ? reinterpret_cast<const uint16_t *>(rows[k])[i] : rows[k][i]) * s.norm;
                if (fallback < 0)
                    fallback = (v - black) / s.t;
                const float w = std::clamp((kSat - v) / (kSat - kFull), 0.f, 1.f) * s.t;
                // least squares with weights ~ t: v - black = t * radiance (+ noise)
                num += w * (v - black);
                den += w * s.t;
            }
            const float r = den > 0 ? num / den : fallback; // radiance at the reference exposure
            o[i] = saturate16((black + std::max(0.f, r)) * 65535.f);
        }
    }
    return out;
}

} // namespace lm
