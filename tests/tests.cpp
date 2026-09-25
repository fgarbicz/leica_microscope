// Unit tests for the imaging core and (optionally, with --hw) the camera driver.
#include "camera/Camera.h"
#include "camera/SimulatedCamera.h"
#include "camera/leica/Dmc6200Camera.h"
#include "imaging/Analysis.h"
#include "imaging/ColorPipeline.h"
#include "imaging/Debayer.h"
#include "imaging/FocusStacker.h"
#include "imaging/MosaicBuilder.h"
#include "imaging/PixelShift.h"
#include "imaging/Registration.h"
#include "imaging/ShadingCorrection.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace lm;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failed;                                                               \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                             \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                          \
    do {                                                                               \
        ++g_checks;                                                                    \
        const double va = (a), vb = (b);                                               \
        if (std::abs(va - vb) > (tol)) {                                               \
            ++g_failed;                                                                \
            std::printf("  FAILED %s:%d: %s = %g, expected %g (+-%g)\n", __FILE__, __LINE__, #a, va, vb, double(tol)); \
        }                                                                              \
    } while (0)

// Band-limited random texture (smooth value noise at several scales), 0..1.
// Defined for fractional coordinates so shifted/subsampled views are exact.
static double lattice(int x, int y, int seed)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0;
}
static double noise(double x, double y, int seed)
{
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    double fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    const double a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
    const double c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}
static double scene(double x, double y)
{
    return 0.15 + 0.35 * noise(x / 23.0, y / 23.0, 1) + 0.3 * noise(x / 9.0, y / 9.0, 2)
           + 0.15 * noise(x / 4.5, y / 4.5, 3);
}

static RawFramePtr makeBayer(int w, int h, PixelFormat fmt, const double rgbGain[3], double dx = 0, double dy = 0)
{
    auto f = std::make_shared<RawFrame>();
    f->width = w;
    f->height = h;
    f->format = fmt;
    f->bitDepth = 12;
    f->stride = w * 2;
    f->data.resize(size_t(w) * h * 2);
    int rx = 0, ry = 0;
    if (fmt == PixelFormat::BayerGR16) { rx = 1; ry = 0; }
    if (fmt == PixelFormat::BayerGB16) { rx = 0; ry = 1; }
    if (fmt == PixelFormat::BayerBG16) { rx = 1; ry = 1; }
    auto *p = reinterpret_cast<uint16_t *>(f->data.data());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool rr = (y & 1) == ry, rc = (x & 1) == rx;
            const int c = rr && rc ? 0 : (!rr && !rc ? 2 : 1);
            p[size_t(y) * w + x] = uint16_t(std::clamp(scene(x + dx, y + dy) * rgbGain[c] * 4095.0, 0.0, 4095.0));
        }
    return f;
}

static void testDemosaic()
{
    std::printf("demosaic\n");
    const double g[3] = {0.9, 0.7, 0.5};
    for (PixelFormat fmt : {PixelFormat::BayerRG16, PixelFormat::BayerGB16, PixelFormat::BayerGR16, PixelFormat::BayerBG16}) {
        auto raw = makeBayer(256, 192, fmt, g);
        for (auto m : {DemosaicMethod::Bilinear, DemosaicMethod::MalvarHeCutler}) {
            Image16 img = toLinearRGB(*raw, m);
            CHECK(img.width == 256 && img.height == 192);
            double err[3] = {0, 0, 0};
            int n = 0;
            for (int y = 8; y < 184; y += 3)
                for (int x = 8; x < 248; x += 3) {
                    for (int c = 0; c < 3; ++c)
                        err[c] += std::abs(img.row(y)[x * 3 + c] / 65535.0 - scene(x, y) * g[c]);
                    ++n;
                }
            for (int c = 0; c < 3; ++c)
                CHECK(err[c] / n < (m == DemosaicMethod::Bilinear ? 0.02 : 0.012));
        }
    }
}

static void testPipeline()
{
    std::printf("colour pipeline\n");
    Image16 img(64, 32);
    for (size_t i = 0; i < img.px.size(); i += 3) {
        img.px[i] = 20000;
        img.px[i + 1] = 30000;
        img.px[i + 2] = 40000;
    }
    ColorPipeline p;
    ColorSettings s;
    s.srgbEncode = false;
    p.update(s);
    Image8 out = p.toDisplay8(img);
    CHECK_NEAR(out.px[0], 20000.0 / 65535 * 255, 1);
    CHECK_NEAR(out.px[2], 40000.0 / 65535 * 255, 1);

    auto wb = computeWhiteBalance(img);
    CHECK_NEAR(wb[0], 1.5, 0.01);
    CHECK_NEAR(wb[2], 0.75, 0.01);
    s.wbRed = wb[0];
    s.wbBlue = wb[2];
    p.update(s);
    Image16 t = img;
    p.applyLinear(t);
    CHECK_NEAR(t.px[0], 30000, 2);
    CHECK_NEAR(t.px[2], 30000, 2);

    // saturation 0 -> grey
    s.saturation = 0;
    s.wbRed = s.wbBlue = 1;
    p.update(s);
    t = img;
    p.applyLinear(t);
    CHECK_NEAR(t.px[0], t.px[1], 2);
    CHECK_NEAR(t.px[1], t.px[2], 2);

    // geometry
    Image16 g(3, 2);
    for (int i = 0; i < 6; ++i)
        g.px[i * 3] = uint16_t(i);
    Image16 r = applyGeometry(g, false, false, 90);
    CHECK(r.width == 2 && r.height == 3);
    CHECK(r.row(0)[0] == 3 && r.row(0)[3] == 0); // top row after 90deg cw: (0,1),(0,0)
    Image16 fh = applyGeometry(g, true, false, 0);
    CHECK(fh.row(0)[0] == 2);
}

static void testRegistration()
{
    std::printf("registration\n");
    for (auto [dx, dy] : std::vector<std::pair<double, double>>{{7, -3}, {-12.5, 4.25}, {30, 18}}) {
        ImageF a(256, 200), b(256, 200);
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 256; ++x) {
                a.at(x, y) = float(scene(x, y));
                b.at(x, y) = float(scene(x + dx, y + dy)); // moving(x) = ref(x + d)
            }
        Shift s = phaseCorrelate(a, b);
        CHECK_NEAR(s.dx, dx, 0.35);
        CHECK_NEAR(s.dy, dy, 0.35);
        CHECK(s.confidence > 0.1);
    }
}

static void testShading()
{
    std::printf("shading\n");
    Image16 flat(400, 300), img(400, 300);
    for (int y = 0; y < 300; ++y)
        for (int x = 0; x < 400; ++x) {
            const double nx = (x - 200) / 200.0, ny = (y - 150) / 150.0;
            const double v = 1.0 - 0.3 * (nx * nx + ny * ny) / 2;
            for (int c = 0; c < 3; ++c) {
                flat.row(y)[x * 3 + c] = uint16_t(50000 * v);
                img.row(y)[x * 3 + c] = uint16_t(30000 * v);
            }
        }
    auto sc = ShadingCorrection::fromReference(flat);
    CHECK(sc && sc->valid());
    ColorPipeline p;
    p.setShading(sc);
    p.applyLinear(img);
    CHECK_NEAR(img.row(0)[0], img.row(150)[200 * 3], 600);
    CHECK_NEAR(img.row(299)[399 * 3], img.row(150)[200 * 3], 600);
}

static void testFocusStack()
{
    std::printf("focus stacking\n");
    // two frames: left half sharp in A, right half sharp in B
    const int w = 256, h = 128;
    auto make = [&](bool leftSharp) {
        Image16 im(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const bool sharp = (x < w / 2) == leftSharp;
                const double v = sharp ? (((x / 3 + y / 3) & 1) ? 0.8 : 0.2) : 0.5;
                for (int c = 0; c < 3; ++c)
                    im.row(y)[x * 3 + c] = uint16_t(v * 65535);
            }
        return im;
    };
    FocusStacker st;
    st.setAlign(false);
    st.add(make(true));
    st.add(make(false));
    Image16 r = st.result();
    CHECK(st.frameCount() == 2);
    // both halves must contain the checkerboard (high contrast)
    auto contrast = [&](int x0, int x1) {
        double mn = 1e9, mx = 0;
        for (int y = 20; y < 100; ++y)
            for (int x = x0; x < x1; ++x) {
                mn = std::min<double>(mn, r.row(y)[x * 3]);
                mx = std::max<double>(mx, r.row(y)[x * 3]);
            }
        return (mx - mn) / 65535.0;
    };
    CHECK(contrast(10, 110) > 0.5);
    CHECK(contrast(146, 246) > 0.5);
}

static void testMosaic()
{
    std::printf("mosaic\n");
    const int w = 320, h = 240;
    auto frameAt = [&](int ox, int oy) {
        Image16 im(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    im.row(y)[x * 3 + c] = uint16_t(scene(x + ox, y + oy) * 60000);
        return im;
    };
    MosaicBuilder mb;
    auto o = mb.options();
    o.maxRestMotion = 1000; // synthetic jumps count as "at rest"
    mb.setOptions(o);
    mb.feed(frameAt(0, 0));
    // move right in steps of 40 px, then down
    for (int s = 1; s <= 6; ++s)
        mb.feed(frameAt(s * 40, 0));
    for (int s = 1; s <= 4; ++s)
        mb.feed(frameAt(240, s * 40));
    auto st = mb.status();
    CHECK(st.tiles >= 3);
    Image16 res = mb.result();
    CHECK_NEAR(res.width, w + 240, 3);
    CHECK_NEAR(res.height, h + 160, 3);
    // compare a pixel in the far corner with the analytic scene
    CHECK_NEAR(res.row(res.height - 10)[(res.width - 10) * 3], scene(res.width - 10, res.height - 10) * 60000, 1500);
}

static void testPixelShift()
{
    std::printf("pixel shift\n");
    const double g[3] = {0.9, 0.7, 0.5};
    std::vector<RawFramePtr> shots;
    std::vector<std::pair<double, double>> offs = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (auto &o : offs)
        shots.push_back(makeBayer(128, 96, PixelFormat::BayerGB16, g, o.first, o.second));
    Image16 r = reconstructPixelShift(shots, offs, {1, 1, 1});
    CHECK(r.width == 128 && r.height == 96);
    double err = 0;
    int n = 0;
    for (int y = 4; y < 90; ++y)
        for (int x = 4; x < 120; ++x)
            for (int c = 0; c < 3; ++c) {
                err += std::abs(r.row(y)[x * 3 + c] / 65535.0 - scene(x, y) * g[c]);
                ++n;
            }
    CHECK(err / n < 0.002); // exact colour at every pixel, only quantisation error
}

static void testSimCamera()
{
    std::printf("simulated camera\n");
    SimulatedCamera cam;
    std::string err;
    CHECK(cam.open(err));
    cam.setResolutionIndex(1);
    std::mutex m;
    std::condition_variable cv;
    int frames = 0;
    cam.setFrameCallback([&](RawFramePtr f) {
        std::lock_guard<std::mutex> l(m);
        if (f && f->width == 1024)
            ++frames;
        cv.notify_all();
    });
    CHECK(cam.startStreaming(err));
    {
        std::unique_lock<std::mutex> l(m);
        cv.wait_for(l, std::chrono::seconds(10), [&] { return frames >= 3; });
    }
    cam.stopStreaming();
    CHECK(frames >= 3);
}

static void testHardware()
{
    std::printf("hardware: Leica DMC6200\n");
    Dmc6200Backend be;
    auto list = be.enumerate();
    if (list.empty()) {
        std::printf("  (no camera connected - skipped)\n");
        return;
    }
    auto cam = be.create(list[0]);
    std::string err;
    CHECK(cam->open(err));
    if (!err.empty())
        std::printf("  open: %s\n", err.c_str());
    std::atomic<int> frames{0};
    std::atomic<double> lastMean{0};
    cam->setFrameCallback([&](RawFramePtr f) {
        auto s = exposureStats(*f, 16);
        lastMean = s.meanLevel;
        ++frames;
    });
    cam->setExposure(10.0);
    CHECK(cam->startStreaming(err));
    auto t0 = std::chrono::steady_clock::now();
    while (frames < 60 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  %d frames in %.2f s (%.1f fps), mean level %.3f\n", frames.load(), dt, frames / dt, lastMean.load());
    CHECK(frames >= 60);
    const double m10 = lastMean;
    cam->setExposure(5.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::printf("  mean at 5 ms: %.3f (10 ms: %.3f)\n", lastMean.load(), m10);
    CHECK_NEAR(lastMean / m10, 0.5, 0.08);
    cam->setGain(2.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::printf("  mean at gain 2: %.3f\n", lastMean.load());
    CHECK_NEAR(lastMean / m10, 1.0, 0.12);
    cam->setGain(1.0);
    cam->stopStreaming();
    // resolution switch
    CHECK(cam->setResolutionIndex(1));
    frames = 0;
    RawFramePtr last;
    std::mutex lm_;
    cam->setFrameCallback([&](RawFramePtr f) {
        std::lock_guard<std::mutex> l(lm_);
        last = f;
        ++frames;
    });
    CHECK(cam->startStreaming(err));
    t0 = std::chrono::steady_clock::now();
    while (frames < 30 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    cam->stopStreaming();
    {
        std::lock_guard<std::mutex> l(lm_);
        std::printf("  ROI frames: %d, size %dx%d\n", frames.load(), last ? last->width : 0, last ? last->height : 0);
        CHECK(last && last->width == 960 && last->height == 600);
    }
    cam->setResolutionIndex(0);
    // 4-shot pixel shift
    std::vector<RawFramePtr> shots;
    bool ok = cam->captureShots(0, shots, err);
    std::printf("  4-shot capture: %s (%zu shots) %s\n", ok ? "ok" : "FAILED", shots.size(), err.c_str());
    CHECK(ok && shots.size() == 4);
    if (ok && shots.size() == 4) {
        // estimate the direction of the sensor motion from shot 0 -> 1 (1 px in x)
        auto gray = [](const RawFrame &f) {
            ImageF g(f.width / 2, f.height / 2);
            auto *p = reinterpret_cast<const uint16_t *>(f.data.data());
            for (int y = 0; y < g.height; ++y)
                for (int x = 0; x < g.width; ++x)
                    g.at(x, y) = float(p[size_t(2 * y) * f.width + 2 * x] + p[size_t(2 * y + 1) * f.width + 2 * x + 1]);
            return g;
        };
        (void)gray;
        // full resolution: demosaic each shot, compare luminance
        ImageF g0 = toGray(toLinearRGB(*shots[0], DemosaicMethod::MalvarHeCutler));
        for (int k = 1; k < 4; ++k) {
            Shift s = phaseCorrelate(g0, toGray(toLinearRGB(*shots[k], DemosaicMethod::MalvarHeCutler)));
            std::printf("  shot %d vs 0: dx=%.3f dy=%.3f px conf=%.3f\n", k, s.dx, s.dy, s.confidence);
        }
        auto modes = cam->shotModes();
        Image16 ps = reconstructPixelShift(shots, modes[0].offsets, {1, -1, -1});
        Image16 single = toLinearRGB(*shots[0], DemosaicMethod::MalvarHeCutler);
        auto savePpm = [](const Image16 &im, const char *path) {
            // simple white-balanced, gamma encoded 8-bit preview
            auto wb = computeWhiteBalance(im);
            FILE *f = std::fopen(path, "wb");
            if (!f)
                return;
            std::fprintf(f, "P6\n%d %d\n255\n", im.width, im.height);
            std::vector<uint8_t> row(size_t(im.width) * 3);
            double peak = 1;
            for (size_t i = 1; i < im.px.size(); i += 3)
                peak = std::max(peak, double(im.px[i]));
            for (int y = 0; y < im.height; ++y) {
                for (int x = 0; x < im.width * 3; ++x) {
                    double v = im.row(y)[x] * wb[x % 3] / (peak * 0.98);
                    row[x] = uint8_t(std::clamp(std::pow(std::clamp(v, 0.0, 1.0), 1 / 2.2) * 255.0, 0.0, 255.0));
                }
                std::fwrite(row.data(), 1, row.size(), f);
            }
            std::fclose(f);
        };
        savePpm(ps, "pixelshift4.ppm");
        savePpm(single, "single.ppm");
        for (size_t k = 0; k < shots.size(); ++k) {
            char name[64];
            std::snprintf(name, sizeof name, "shot4_%zu.raw", k);
            if (FILE *f = std::fopen(name, "wb")) {
                std::fwrite(shots[k]->data.data(), 1, shots[k]->data.size(), f);
                std::fclose(f);
            }
        }
        std::printf("  saved pixelshift4.ppm / single.ppm / shot4_*.raw\n");
    }
    cam->close();
}

int main(int argc, char **argv)
{
    const bool hw = argc > 1 && std::strcmp(argv[1], "--hw") == 0;
    const bool only = argc > 2 && std::strcmp(argv[2], "--only") == 0;
    if (!only) {
        testDemosaic();
        testPipeline();
        testRegistration();
        testShading();
        testFocusStack();
        testMosaic();
        testPixelShift();
        testSimCamera();
    }
    if (hw)
        testHardware();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
