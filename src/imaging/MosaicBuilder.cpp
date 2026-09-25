#include "MosaicBuilder.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cmath>

namespace lm {

void MosaicBuilder::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status = {};
    m_canvas = {};
    m_weight.clear();
    m_originX = m_originY = 0;
    m_prevGray = {};
    m_posX = m_posY = 0;
    m_lost = false;
    m_tiles.clear();
    m_frameW = m_frameH = 0;
}

void MosaicBuilder::setOptions(const Options &o)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_opt = o;
}

MosaicBuilder::Options MosaicBuilder::options() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_opt;
}

MosaicBuilder::Status MosaicBuilder::status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void MosaicBuilder::ensureCanvas(int x0, int y0, int x1, int y1)
{
    // requested region in mosaic coordinates [x0,x1) x [y0,y1)
    const int cx0 = m_originX, cy0 = m_originY;
    const int cx1 = m_originX + m_canvas.width, cy1 = m_originY + m_canvas.height;
    if (!m_canvas.empty() && x0 >= cx0 && y0 >= cy0 && x1 <= cx1 && y1 <= cy1)
        return;
    const int marginX = m_frameW, marginY = m_frameH;
    int nx0 = m_canvas.empty() ? x0 : std::min(cx0, x0 < cx0 ? x0 - marginX : cx0);
    int ny0 = m_canvas.empty() ? y0 : std::min(cy0, y0 < cy0 ? y0 - marginY : cy0);
    int nx1 = m_canvas.empty() ? x1 : std::max(cx1, x1 > cx1 ? x1 + marginX : cx1);
    int ny1 = m_canvas.empty() ? y1 : std::max(cy1, y1 > cy1 ? y1 + marginY : cy1);
    Image16 canvas(nx1 - nx0, ny1 - ny0);
    std::vector<uint8_t> weight(size_t(canvas.width) * canvas.height, 0);
    if (!m_canvas.empty()) {
        const int ox = cx0 - nx0, oy = cy0 - ny0;
        for (int y = 0; y < m_canvas.height; ++y) {
            std::copy_n(m_canvas.row(y), size_t(m_canvas.width) * 3, canvas.row(y + oy) + size_t(ox) * 3);
            std::copy_n(m_weight.data() + size_t(y) * m_canvas.width, m_canvas.width,
                        weight.data() + size_t(y + oy) * canvas.width + ox);
        }
    }
    m_canvas = std::move(canvas);
    m_weight = std::move(weight);
    m_originX = nx0;
    m_originY = ny0;
}

void MosaicBuilder::paste(const Image16 &frame, int px, int py)
{
    ensureCanvas(px, py, px + frame.width, py + frame.height);
    const int ox = px - m_originX, oy = py - m_originY;
    const float feather = float(std::max(1, m_opt.featherPixels));
    parallelRows(frame.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint16_t *s = frame.row(y);
            uint16_t *d = m_canvas.row(y + oy) + size_t(ox) * 3;
            uint8_t *wrow = m_weight.data() + size_t(y + oy) * m_canvas.width + ox;
            const int ey = std::min(y, frame.height - 1 - y);
            for (int x = 0; x < frame.width; ++x) {
                const int e = std::min(ey, std::min(x, frame.width - 1 - x));
                const float wn = std::clamp((e + 1) / feather, 1.f / 255.f, 1.f);
                const uint8_t wo8 = wrow[x];
                if (wo8 == 0) {
                    d[x * 3] = s[x * 3];
                    d[x * 3 + 1] = s[x * 3 + 1];
                    d[x * 3 + 2] = s[x * 3 + 2];
                    wrow[x] = uint8_t(std::max(1.f, wn * 255.f));
                } else {
                    const float wo = wo8 / 255.f;
                    const float a = wn / (wn + wo);
                    for (int c = 0; c < 3; ++c)
                        d[x * 3 + c] = uint16_t(d[x * 3 + c] + a * (float(s[x * 3 + c]) - d[x * 3 + c]) + 0.5f);
                    wrow[x] = uint8_t(std::max(1.f, std::max(wo, wn) * 255.f));
                }
            }
        }
    });
    m_tiles.push_back({px, py, frame.width, frame.height});
}

double MosaicBuilder::uncoveredFraction(int x, int y, int w, int h) const
{
    uint64_t total = 0, empty = 0;
    for (int yy = y; yy < y + h; yy += 8) {
        for (int xx = x; xx < x + w; xx += 8) {
            ++total;
            const int cx = xx - m_originX, cy = yy - m_originY;
            if (cx < 0 || cy < 0 || cx >= m_canvas.width || cy >= m_canvas.height
                || m_weight[size_t(cy) * m_canvas.width + cx] == 0)
                ++empty;
        }
    }
    return total ? double(empty) / total : 1.0;
}

ImageF MosaicBuilder::canvasGray(int x, int y, int w, int h, int f) const
{
    ImageF out(w / f, h / f);
    double sum = 0;
    uint64_t n = 0;
    std::vector<uint8_t> valid(out.px.size(), 0);
    for (int gy = 0; gy < out.height; ++gy)
        for (int gx = 0; gx < out.width; ++gx) {
            float acc = 0;
            int cnt = 0;
            for (int dy = 0; dy < f; ++dy)
                for (int dx = 0; dx < f; ++dx) {
                    const int cx = x + gx * f + dx - m_originX, cy = y + gy * f + dy - m_originY;
                    if (cx < 0 || cy < 0 || cx >= m_canvas.width || cy >= m_canvas.height
                        || m_weight[size_t(cy) * m_canvas.width + cx] == 0)
                        continue;
                    const uint16_t *p = m_canvas.row(cy) + size_t(cx) * 3;
                    acc += 0.25f * p[0] + 0.5f * p[1] + 0.25f * p[2];
                    ++cnt;
                }
            if (cnt == f * f) {
                const float v = acc / (cnt * 65535.f);
                out.at(gx, gy) = v;
                valid[size_t(gy) * out.width + gx] = 1;
                sum += v;
                ++n;
            }
        }
    const float mean = n ? float(sum / n) : 0.f;
    for (size_t i = 0; i < out.px.size(); ++i)
        if (!valid[i])
            out.px[i] = mean;
    return out;
}

MosaicBuilder::Status MosaicBuilder::feed(const Image16 &frame, bool forceAdd)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Status st = m_status;
    st.added = false;
    if (frame.empty())
        return st;
    if (!m_tiles.empty() && (frame.width != m_frameW || frame.height != m_frameH)) {
        st.tracking = false;
        m_status = st;
        return st;
    }
    m_frameW = frame.width;
    m_frameH = frame.height;
    m_factor = std::max(1, frame.width / 384);
    const int f = m_factor;
    ImageF g = toGray(frame, f);

    if (m_tiles.empty()) {
        paste(frame, 0, 0);
        m_posX = m_posY = 0;
        m_prevGray = std::move(g);
        st = {};
        st.tracking = true;
        st.added = true;
        st.confidence = 1.0;
        st.tiles = 1;
        m_status = st;
        return st;
    }

    // 1. frame-to-frame tracking
    Shift s = phaseCorrelate(m_prevGray, g);
    bool tracking = s.confidence >= m_opt.minConfidence;
    double motion = std::hypot(s.dx, s.dy);
    if (tracking && !m_lost) {
        m_posX += s.dx * f;
        m_posY += s.dy * f;
    } else {
        // 2. relocalise against the mosaic at the last known position
        ImageF ref = canvasGray(int(std::lround(m_posX)), int(std::lround(m_posY)), frame.width, frame.height, f);
        Shift r = phaseCorrelate(ref, g);
        if (r.confidence >= m_opt.minConfidence * 1.5) {
            m_posX += r.dx * f;
            m_posY += r.dy * f;
            tracking = true;
            motion = 0.0;
            s = r;
        } else {
            tracking = false;
        }
    }
    m_lost = !tracking;
    m_prevGray = std::move(g);

    st.tracking = tracking;
    st.confidence = s.confidence;
    const int ix = int(std::lround(m_posX)), iy = int(std::lround(m_posY));
    st.newAreaFraction = uncoveredFraction(ix, iy, frame.width, frame.height);

    const bool atRest = motion <= m_opt.maxRestMotion;
    const bool wantAdd = forceAdd || (m_opt.autoAdd && atRest && st.newAreaFraction >= m_opt.minNewArea);
    if (tracking && wantAdd && st.newAreaFraction < 0.97) {
        // 3. refine against the existing mosaic to remove accumulated drift
        ImageF ref = canvasGray(ix, iy, frame.width, frame.height, f);
        ImageF cur = toGray(frame, f);
        Shift r = phaseCorrelate(ref, cur);
        double px = m_posX, py = m_posY;
        if (r.confidence >= m_opt.minConfidence && std::abs(r.dx * f) < frame.width * 0.25
            && std::abs(r.dy * f) < frame.height * 0.25) {
            px += r.dx * f;
            py += r.dy * f;
        }
        m_posX = px;
        m_posY = py;
        paste(frame, int(std::lround(px)), int(std::lround(py)));
        st.added = true;
    } else if (forceAdd && st.newAreaFraction >= 0.97) {
        // cannot place a tile with no overlap
        st.added = false;
    }
    // report the position relative to the cropped mosaic (see result())
    int minX = INT32_MAX, minY = INT32_MAX;
    for (const auto &t : m_tiles) {
        minX = std::min(minX, t.x);
        minY = std::min(minY, t.y);
    }
    st.posX = m_posX - minX;
    st.posY = m_posY - minY;
    st.tiles = int(m_tiles.size());
    m_status = st;
    return st;
}

Image16 MosaicBuilder::result(uint16_t bg) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_canvas.empty())
        return {};
    int minX = m_canvas.width, minY = m_canvas.height, maxX = -1, maxY = -1;
    for (const auto &t : m_tiles) {
        minX = std::min(minX, t.x - m_originX);
        minY = std::min(minY, t.y - m_originY);
        maxX = std::max(maxX, t.x - m_originX + t.w);
        maxY = std::max(maxY, t.y - m_originY + t.h);
    }
    Image16 out(maxX - minX, maxY - minY);
    for (int y = 0; y < out.height; ++y) {
        const uint16_t *s = m_canvas.row(y + minY) + size_t(minX) * 3;
        const uint8_t *w = m_weight.data() + size_t(y + minY) * m_canvas.width + minX;
        uint16_t *d = out.row(y);
        for (int x = 0; x < out.width; ++x) {
            if (w[x]) {
                d[x * 3] = s[x * 3];
                d[x * 3 + 1] = s[x * 3 + 1];
                d[x * 3 + 2] = s[x * 3 + 2];
            } else {
                d[x * 3] = d[x * 3 + 1] = d[x * 3 + 2] = bg;
            }
        }
    }
    return out;
}

Image16 MosaicBuilder::preview(int maxSize, double &scale, uint16_t bg) const
{
    Image16 full = result(bg);
    if (full.empty()) {
        scale = 1.0;
        return full;
    }
    const int f = std::max(1, (std::max(full.width, full.height) + maxSize - 1) / maxSize);
    scale = 1.0 / f;
    if (f == 1)
        return full;
    Image16 out(full.width / f, full.height / f);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            const uint16_t *s = full.row(y * f) + size_t(x) * f * 3;
            uint16_t *d = out.row(y) + size_t(x) * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    return out;
}

std::vector<MosaicBuilder::TileRect> MosaicBuilder::tiles() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_tiles.empty())
        return {};
    int minX = INT32_MAX, minY = INT32_MAX;
    for (const auto &t : m_tiles) {
        minX = std::min(minX, t.x);
        minY = std::min(minY, t.y);
    }
    std::vector<TileRect> out = m_tiles;
    for (auto &t : out) {
        t.x -= minX;
        t.y -= minY;
    }
    return out;
}

} // namespace lm
