#include "FocusStacker.h"

#include "core/Parallel.h"
#include "imaging/Analysis.h"
#include "imaging/Registration.h"

#include <algorithm>
#include <cmath>

namespace lm {

namespace {

// Translates an image by an integer offset (edges replicated).
Image16 translate(const Image16 &in, int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return in;
    Image16 out(in.width, in.height);
    parallelRows(in.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int sy = std::clamp(y - dy, 0, in.height - 1);
            const uint16_t *s = in.row(sy);
            uint16_t *d = out.row(y);
            for (int x = 0; x < in.width; ++x) {
                const int sx = std::clamp(x - dx, 0, in.width - 1);
                d[x * 3] = s[sx * 3];
                d[x * 3 + 1] = s[sx * 3 + 1];
                d[x * 3 + 2] = s[sx * 3 + 2];
            }
        }
    });
    return out;
}

int regFactor(int w) { return std::max(1, w / 512); }

} // namespace

void FocusStacker::setMode(Mode m)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mode = m;
}

FocusStacker::Mode FocusStacker::mode() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mode;
}

void FocusStacker::setAlign(bool on)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_align = on;
}

void FocusStacker::setWindow(int radius)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_window = radius;
}

void FocusStacker::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_count = 0;
    m_composite = {};
    m_best = {};
    m_depth = {};
    m_accum.clear();
    m_weights.clear();
    m_refGray = {};
}

double FocusStacker::add(const Image16 &frameIn)
{
    if (frameIn.empty())
        return 0.0;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_count > 0 && (frameIn.width != m_composite.width || frameIn.height != m_composite.height))
        return 0.0; // resolution changed mid-stack; ignore

    Image16 frame = frameIn;
    const int f = regFactor(frameIn.width);
    if (m_align) {
        ImageF g = toGray(frameIn, f);
        // alignment switched on mid-stack: this frame becomes the reference
        if (m_count == 0 || m_refGray.px.empty()) {
            m_refGray = std::move(g);
        } else {
            Shift s = phaseCorrelate(m_refGray, g);
            if (s.confidence > 0.03) {
                // moving(x) ~ ref(x + d)  =>  aligned(x) = moving(x - d)
                const int dx = int(std::lround(s.dx * f)), dy = int(std::lround(s.dy * f));
                if (std::abs(dx) < frame.width / 4 && std::abs(dy) < frame.height / 4)
                    frame = translate(frame, dx, dy);
            }
        }
    }

    ImageF sharp = localSharpness(frame, m_window);
    const int w = frame.width, h = frame.height;
    const size_t n = size_t(w) * h;
    const int index = m_count;

    if (m_count == 0) {
        m_composite = frame;
        m_best = sharp;
        m_depth = ImageF(w, h, 0.f);
        m_stackMode = m_mode;
        if (m_stackMode == Mode::Weighted) {
            m_accum.assign(n * 3, 0.f);
            m_weights.assign(n, 0.f);
        }
    }

    std::vector<uint32_t> improvedPerRow(h, 0);
    const Mode mode = m_stackMode;
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint16_t *src = frame.row(y);
            uint16_t *dst = m_composite.row(y);
            uint32_t improved = 0;
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                const float s = sharp.px[i];
                if (m_count == 0 || s > m_best.px[i]) {
                    if (m_count > 0)
                        ++improved;
                    m_best.px[i] = s;
                    m_depth.px[i] = float(index);
                    if (mode == Mode::MaxContrast) {
                        dst[x * 3] = src[x * 3];
                        dst[x * 3 + 1] = src[x * 3 + 1];
                        dst[x * 3 + 2] = src[x * 3 + 2];
                    }
                }
                if (mode == Mode::Weighted) {
                    const float wgt = s * s + 1e-3f;
                    m_accum[i * 3] += wgt * src[x * 3];
                    m_accum[i * 3 + 1] += wgt * src[x * 3 + 1];
                    m_accum[i * 3 + 2] += wgt * src[x * 3 + 2];
                    m_weights[i] += wgt;
                }
            }
            improvedPerRow[y] = improved;
        }
    });
    ++m_count;
    uint64_t total = 0;
    for (auto v : improvedPerRow)
        total += v;
    return index == 0 ? 1.0 : double(total) / n;
}

int FocusStacker::frameCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_count;
}

Image16 FocusStacker::result() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stackMode == Mode::MaxContrast || m_weights.empty())
        return m_composite;
    Image16 out(m_composite.width, m_composite.height);
    const size_t n = size_t(out.width) * out.height;
    for (size_t i = 0; i < n; ++i) {
        const float wsum = std::max(m_weights[i], 1e-6f);
        for (int c = 0; c < 3; ++c)
            out.px[i * 3 + c] = uint16_t(std::clamp(m_accum[i * 3 + c] / wsum, 0.f, 65535.f));
    }
    return out;
}

Image16 FocusStacker::depthMap() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Image16 out(m_depth.width, m_depth.height);
    const float scale = m_count > 1 ? 65535.f / (m_count - 1) : 0.f;
    for (size_t i = 0; i < m_depth.px.size(); ++i) {
        const uint16_t v = uint16_t(m_depth.px[i] * scale);
        out.px[i * 3] = out.px[i * 3 + 1] = out.px[i * 3 + 2] = v;
    }
    return out;
}

} // namespace lm
