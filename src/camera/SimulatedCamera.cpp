#include "SimulatedCamera.h"

#include "core/Parallel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace lm {

namespace {

constexpr int kFullW = 2048, kFullH = 1536;
constexpr int kSlideW = 6144, kSlideH = 4608;
constexpr double kPi = 3.14159265358979;

inline uint32_t hash2(int x, int y, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline float rnd01(int x, int y, uint32_t seed) { return (hash2(x, y, seed) & 0xFFFFFF) / float(0x1000000); }

// smooth value noise in [0,1]
float valueNoise(float x, float y, uint32_t seed)
{
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = rnd01(ix, iy, seed), b = rnd01(ix + 1, iy, seed);
    float c = rnd01(ix, iy + 1, seed), d = rnd01(ix + 1, iy + 1, seed);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

float fbm(float x, float y, uint32_t seed, int octaves)
{
    float s = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < octaves; ++o) {
        s += amp * valueNoise(x, y, seed + o * 17);
        norm += amp;
        x *= 2.03f;
        y *= 2.03f;
        amp *= 0.5f;
    }
    return s / norm;
}

struct XorShift {
    uint64_t s;
    explicit XorShift(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return uint32_t(s >> 11);
    }
    float uniform() { return (next() & 0xFFFFFF) / float(0x1000000); }
    // approx. standard normal (Irwin-Hall, 4 terms)
    float normal() { return (uniform() + uniform() + uniform() + uniform() - 2.f) * 1.7320508f; }
};

void boxBlur(std::vector<float> &buf, int w, int h, int x0, int x1, int r)
{
    if (r <= 0)
        return;
    // horizontal then vertical box blur restricted to columns [x0, x1)
    std::vector<float> tmp(buf.size());
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float *row = buf.data() + size_t(y) * w;
            float *o = tmp.data() + size_t(y) * w;
            for (int x = x0; x < x1; ++x) {
                float s = 0;
                int cnt = 0;
                for (int k = -r; k <= r; ++k) {
                    int xx = std::clamp(x + k, 0, w - 1);
                    s += row[xx];
                    ++cnt;
                }
                o[x] = s / cnt;
            }
        }
    });
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float *o = buf.data() + size_t(y) * w;
            for (int x = x0; x < x1; ++x) {
                float s = 0;
                int cnt = 0;
                for (int k = -r; k <= r; ++k) {
                    int yy = std::clamp(y + k, 0, h - 1);
                    s += tmp[size_t(yy) * w + x];
                    ++cnt;
                }
                o[x] = s / cnt;
            }
        }
    });
}

} // namespace

SimulatedCamera::SimulatedCamera() = default;

SimulatedCamera::~SimulatedCamera()
{
    close();
}

CameraInfo SimulatedCamera::info() const
{
    return {"sim:0", "Simulated DM2000 camera (IHC gastric mucosa)", "Simulator", "SIM-0001", "Simulator"};
}

bool SimulatedCamera::open(std::string &)
{
    if (m_open)
        return true;
    buildSlide();
    m_stageX = (kSlideW - kFullW) / 2.0;
    m_stageY = (kSlideH - kFullH) / 2.0;
    m_open = true;
    return true;
}

void SimulatedCamera::close()
{
    stopStreaming();
    m_open = false;
}

bool SimulatedCamera::startStreaming(std::string &error)
{
    if (!m_open) {
        error = "camera not open";
        return false;
    }
    if (m_streaming)
        return true;
    m_streaming = true;
    m_thread = std::thread([this] {
        try {
            run();
        } catch (...) {
            m_streaming = false;
            emitCurrentException("Simulated stream stopped: ");
        }
    });
    return true;
}

void SimulatedCamera::stopStreaming()
{
    m_streaming = false;
    if (m_thread.joinable())
        m_thread.join();
}

std::vector<Resolution> SimulatedCamera::resolutions() const
{
    return {{kFullW, kFullH, 1, "2048 x 1536 (full)"}, {kFullW / 2, kFullH / 2, 2, "1024 x 768 (bin 2x2)"}};
}

bool SimulatedCamera::setResolutionIndex(int index)
{
    if (index < 0 || index > 1)
        return false;
    m_resIndex = index;
    return true;
}

bool SimulatedCamera::setExposure(double ms)
{
    m_exposure = std::clamp(ms, exposureRange().min, exposureRange().max);
    return true;
}

bool SimulatedCamera::setGain(double g)
{
    m_gain = std::clamp(g, gainRange().min, gainRange().max);
    return true;
}

bool SimulatedCamera::setBitDepth(int b)
{
    if (b != 8 && b != 12)
        return false;
    m_bitDepth = b;
    return true;
}

std::vector<CameraProperty> SimulatedCamera::properties() const
{
    using T = CameraProperty::Type;
    return {
        {"stage_x", "Stage X (px)", T::Number, 0, double(kSlideW - kFullW), 10, m_stageX.load()},
        {"stage_y", "Stage Y (px)", T::Number, 0, double(kSlideH - kFullH), 10, m_stageY.load()},
        {"focus", "Focus offset", T::Number, -20, 20, 0.5, m_focus.load()},
        {"tilt", "Section tilt", T::Number, 0, 20, 0.5, m_tilt.load()},
        {"lamp", "Lamp intensity", T::Number, 0.05, 2.0, 0.05, m_lamp.load()},
        {"drift_x", "Stage drift X (px/frame)", T::Number, -40, 40, 1, m_driftX.load()},
        {"drift_y", "Stage drift Y (px/frame)", T::Number, -40, 40, 1, m_driftY.load()},
    };
}

bool SimulatedCamera::setProperty(const std::string &key, double v)
{
    if (key == "stage_x") m_stageX = std::clamp(v, 0.0, double(kSlideW - kFullW));
    else if (key == "stage_y") m_stageY = std::clamp(v, 0.0, double(kSlideH - kFullH));
    else if (key == "focus") m_focus = v;
    else if (key == "tilt") m_tilt = v;
    else if (key == "lamp") m_lamp = v;
    else if (key == "drift_x") m_driftX = v;
    else if (key == "drift_y") m_driftY = v;
    else return false;
    return true;
}

void SimulatedCamera::buildSlide()
{
    std::lock_guard<std::mutex> lock(m_slideMutex);
    if (!m_hema.empty())
        return;
    const int W = kSlideW, H = kSlideH;
    m_slideW = W;
    m_slideH = H;
    m_hema.assign(size_t(W) * H, 0);
    m_dab.assign(size_t(W) * H, 0);
    m_eosin.assign(size_t(W) * H, 0);

    // tissue mask & stroma texture from low-frequency noise
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                float edge = fbm(x / 900.f, y / 900.f, 7, 3) + 0.25f * (1.f - float(x) / W);
                if (edge < 0.42f)
                    continue; // glass (background)
                float fibre = fbm(x / 22.f, y / 60.f, 11, 4);
                float tex = fbm(x / 6.f, y / 6.f, 13, 2);
                m_eosin[i] = uint8_t(std::clamp(30.f + 70.f * fibre + 20.f * tex, 0.f, 255.f));
                m_hema[i] = uint8_t(std::clamp(8.f + 18.f * fibre, 0.f, 255.f));
            }
    });

    auto inTissue = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < W && y < H && m_eosin[size_t(y) * W + x] > 0;
    };
    auto blob = [&](std::vector<uint8_t> &ch, float cx, float cy, float rx, float ry, float ang, float peak) {
        const float c = std::cos(ang), s = std::sin(ang);
        const int r = int(std::ceil(std::max(rx, ry))) + 1;
        for (int y = int(cy) - r; y <= int(cy) + r; ++y)
            for (int x = int(cx) - r; x <= int(cx) + r; ++x) {
                if (!inTissue(x, y))
                    continue;
                float dx = x - cx, dy = y - cy;
                float u = (dx * c + dy * s) / rx, v = (-dx * s + dy * c) / ry;
                float d2 = u * u + v * v;
                if (d2 > 1.f)
                    continue;
                float val = peak * (1.f - 0.35f * d2) * (0.8f + 0.4f * rnd01(x, y, 99));
                auto &p = ch[size_t(y) * W + x];
                p = uint8_t(std::min(255.f, std::max(float(p), val)));
            }
    };

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> U(0.f, 1.f);
    // gastric glands on a jittered grid
    const int spacing = 230;
    for (int gy = 0; gy < H / spacing + 1; ++gy)
        for (int gx = 0; gx < W / spacing + 1; ++gx) {
            float cx = gx * spacing + spacing * (0.3f + 0.4f * U(rng));
            float cy = gy * spacing + spacing * (0.3f + 0.4f * U(rng));
            if (!inTissue(int(cx), int(cy)))
                continue;
            float rx = 70.f + 40.f * U(rng), ry = 45.f + 30.f * U(rng);
            float ang = float(kPi) * U(rng);
            const float thick = 32.f + 10.f * U(rng);
            const bool positive = U(rng) < 0.4f;       // DAB positive cytoplasm
            const float dabLevel = 90.f + 120.f * U(rng);
            const float c = std::cos(ang), s = std::sin(ang);
            const int R = int(std::max(rx, ry) + thick) + 2;
            // epithelium ring + lumen
            for (int y = int(cy) - R; y <= int(cy) + R; ++y)
                for (int x = int(cx) - R; x <= int(cx) + R; ++x) {
                    if (!inTissue(x, y))
                        continue;
                    float dx = x - cx, dy = y - cy;
                    float u = (dx * c + dy * s), v = (-dx * s + dy * c);
                    float rin = std::sqrt((u / rx) * (u / rx) + (v / ry) * (v / ry));
                    float rout = std::sqrt((u / (rx + thick)) * (u / (rx + thick)) + (v / (ry + thick)) * (v / (ry + thick)));
                    const size_t i = size_t(y) * W + x;
                    if (rin < 1.f) { // lumen: clear
                        m_eosin[i] = uint8_t(m_eosin[i] * 0.1f);
                        m_hema[i] = uint8_t(m_hema[i] * 0.1f);
                    } else if (rout < 1.f) { // cytoplasm
                        float t = fbm(x / 5.f, y / 5.f, 21, 2);
                        m_eosin[i] = uint8_t(std::clamp(60.f + 60.f * t, 0.f, 255.f));
                        m_hema[i] = uint8_t(std::clamp(25.f + 15.f * t, 0.f, 255.f));
                        if (positive)
                            m_dab[i] = uint8_t(std::clamp(dabLevel * (0.6f + 0.6f * t), 0.f, 255.f));
                    }
                }
            // basal nuclei along the outer part of the ring
            const int nNuc = int(2 * kPi * (rx + ry) / 2 / 13);
            for (int k = 0; k < nNuc; ++k) {
                float a = float(2 * kPi) * (k + 0.3f * U(rng)) / nNuc;
                float rr = 1.f + (thick * (0.55f + 0.25f * U(rng))) / ((rx + ry) / 2);
                float u = std::cos(a) * rx * rr, v = std::sin(a) * ry * rr;
                float nx = cx + u * c - v * s, ny = cy + u * s + v * c;
                bool ki67 = U(rng) < 0.12f;
                blob(ki67 ? m_dab : m_hema, nx, ny, 7.f + 2.f * U(rng), 4.5f + 1.5f * U(rng), a, ki67 ? 230.f : 200.f);
                if (ki67)
                    blob(m_hema, nx, ny, 6.f, 4.f, a, 60.f);
            }
        }
    // scattered stromal / inflammatory cells
    for (int k = 0; k < 9000; ++k) {
        float x = U(rng) * W, y = U(rng) * H;
        if (!inTissue(int(x), int(y)) || m_dab[size_t(y) * W + size_t(x)] > 0)
            continue;
        float r = 3.5f + 2.f * U(rng);
        blob(m_hema, x, y, r, r * (0.8f + 0.3f * U(rng)), float(kPi) * U(rng), 210.f);
    }
}

RawFramePtr SimulatedCamera::renderFrame(uint64_t seq)
{
    const auto res = resolutions()[std::clamp(m_resIndex.load(), 0, 1)];
    const int bin = res.binning;
    const int w = res.width, h = res.height;

    // advance stage drift
    if (m_driftX != 0.0 || m_driftY != 0.0) {
        m_stageX = std::clamp(m_stageX + m_driftX, 0.0, double(kSlideW - kFullW));
        m_stageY = std::clamp(m_stageY + m_driftY, 0.0, double(kSlideH - kFullH));
    }
    const int sx0 = int(m_stageX), sy0 = int(m_stageY);

    // crop stain maps (averaging bins)
    std::vector<float> hm(size_t(w) * h), db(size_t(w) * h), eo(size_t(w) * h);
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float a = 0, b = 0, c = 0;
                for (int dy = 0; dy < bin; ++dy)
                    for (int dx = 0; dx < bin; ++dx) {
                        const int X = std::clamp(sx0 + x * bin + dx, 0, m_slideW - 1);
                        const int Y = std::clamp(sy0 + y * bin + dy, 0, m_slideH - 1);
                        const size_t i = size_t(Y) * m_slideW + X;
                        a += m_hema[i];
                        b += m_dab[i];
                        c += m_eosin[i];
                    }
                const float n = float(bin * bin);
                const size_t o = size_t(y) * w + x;
                hm[o] = a / n;
                db[o] = b / n;
                eo[o] = c / n;
            }
    });

    // defocus: 6 vertical bands with depth varying across the section
    const int bands = 6;
    for (int b = 0; b < bands; ++b) {
        const int x0 = w * b / bands, x1 = w * (b + 1) / bands;
        const double depth = m_tilt * ((b + 0.5) / bands - 0.5);
        const int r = int(std::lround(std::abs(m_focus + depth) * 1.5 / bin));
        boxBlur(hm, w, h, x0, x1, r);
        boxBlur(db, w, h, x0, x1, r);
        boxBlur(eo, w, h, x0, x1, r);
    }

    // Beer-Lambert: optical density vectors (Ruifrok & Johnston), halogen lamp
    const float odH[3] = {0.65f, 0.70f, 0.29f};
    const float odD[3] = {0.27f, 0.57f, 0.78f};
    const float odE[3] = {0.07f, 0.99f, 0.11f};
    const float lampRGB[3] = {1.0f, 0.82f, 0.55f};
    const float scale = 1.4f / 255.f;
    const double fullWell = 10000.0;
    const double electronsPerMs = 900.0 * m_lamp;
    const double exposure = m_exposure, gainv = m_gain;
    const int bd = m_bitDepth;
    const double maxDN = (1 << bd) - 1;
    const double black = bd == 12 ? 64.0 : 4.0;

    auto frame = std::make_shared<RawFrame>();
    frame->width = w;
    frame->height = h;
    frame->bitDepth = bd;
    frame->format = bd > 8 ? PixelFormat::BayerRG16 : PixelFormat::BayerRG8;
    frame->stride = w * (bd > 8 ? 2 : 1);
    frame->data.resize(size_t(frame->stride) * h);
    frame->sequence = seq;
    frame->timestamp = std::chrono::steady_clock::now();
    frame->exposureMs = exposure;
    frame->gain = gainv;

    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            XorShift rng(seq * 100003ull + y);
            const float ny = (y - h / 2.f) / (h / 2.f);
            for (int x = 0; x < w; ++x) {
                const int ch = (y & 1) == 0 ? ((x & 1) == 0 ? 0 : 1) : ((x & 1) == 0 ? 1 : 2);
                const size_t i = size_t(y) * w + x;
                const float od = scale * (hm[i] * odH[ch] + db[i] * odD[ch] + eo[i] * odE[ch]);
                const float nx = (x - w / 2.f) / (w / 2.f);
                const float vign = 1.f - 0.28f * (nx * nx * 0.56f + ny * ny * 0.56f + 0.12f * nx);
                double e = std::exp(-od) * vign * lampRGB[ch] * electronsPerMs * exposure * bin * bin / 4.0;
                e = std::min(e, fullWell * 1.2);
                e += std::sqrt(std::max(e, 1.0)) * rng.normal() + 6.0 * rng.normal();
                double dn = black + e * gainv * (maxDN / fullWell);
                dn = std::clamp(dn, 0.0, maxDN);
                if (bd > 8)
                    reinterpret_cast<uint16_t *>(frame->data.data() + size_t(y) * frame->stride)[x] = uint16_t(dn + 0.5);
                else
                    frame->data[size_t(y) * frame->stride + x] = uint8_t(dn + 0.5);
            }
        }
    });
    return frame;
}

void SimulatedCamera::run()
{
    uint64_t seq = 0;
    auto next = std::chrono::steady_clock::now();
    while (m_streaming) {
        const double periodMs = std::max(m_exposure.load(), m_resIndex == 0 ? 33.3 : 16.7);
        next += std::chrono::microseconds(int64_t(periodMs * 1000));
        auto frame = renderFrame(seq++);
        emitFrame(frame);
        auto now = std::chrono::steady_clock::now();
        if (next > now) {
            // sleep in small steps so stopStreaming() stays responsive
            while (m_streaming && std::chrono::steady_clock::now() < next)
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(
                    10, std::chrono::duration_cast<std::chrono::milliseconds>(next - std::chrono::steady_clock::now()).count() + 1)));
        } else {
            next = now;
        }
    }
}

} // namespace lm
