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
#include "imaging/StainAnalysis.h"
#include "imaging/NucleusDetection.h"

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

    // camera colour correction (DMC6200 / IMX174): neutral stays neutral, colours change
    {
        const auto ccm = Dmc6200Camera(CameraInfo{}).colorMatrix();
        for (int i = 0; i < 3; ++i)
            CHECK_NEAR(ccm[size_t(i * 3)] + ccm[size_t(i * 3 + 1)] + ccm[size_t(i * 3 + 2)], 1.0, 1e-3);
        ColorSettings cs;
        cs.srgbEncode = false;
        cs.cameraMatrix = ccm;
        ColorPipeline cp;
        cp.update(cs);
        Image16 patch(2, 1);
        const uint16_t grey[3] = {30000, 30000, 30000}, tint[3] = {40000, 30000, 20000};
        for (int c = 0; c < 3; ++c) {
            patch.row(0)[c] = grey[c];
            patch.row(0)[3 + c] = tint[c];
        }
        Image16 corrected = patch;
        cp.applyLinear(corrected);
        CHECK_NEAR(corrected.row(0)[0], 30000, 40);
        CHECK_NEAR(corrected.row(0)[1], 30000, 40);
        CHECK_NEAR(corrected.row(0)[2], 30000, 40);
        CHECK(std::abs(int(corrected.row(0)[3]) - 40000) > 1000); // colour saturation raised
        cs.colorCorrection = false;
        cp.update(cs);
        Image16 plain = patch;
        cp.applyLinear(plain);
        CHECK(plain.row(0)[3] == 40000 && plain.row(0)[5] == 20000);
    }
}

static void testFastPreview()
{
    std::printf("fast preview == reference pipeline\n");
    const double g[3] = {0.8, 0.6, 0.4};
    auto raw = makeBayer(200, 120, PixelFormat::BayerGB16, g);
    for (int variant = 0; variant < 3; ++variant) {
        ColorSettings s;
        s.wbRed = 1.3;
        s.wbBlue = 1.7;
        s.saturation = 1.4;
        s.gamma = 1.2;
        s.blackLevel = 0.01;
        if (variant == 1)
            s.rotation = 90;
        if (variant == 2) {
            s.flipHorizontal = true;
            s.rotation = 270;
        }
        ColorPipeline p;
        p.update(s);
        Image8 ref = p.render8(toLinearRGB(*raw, DemosaicMethod::Bilinear));
        int w, h;
        ColorPipeline::previewSize(*raw, s.rotation, w, h);
        CHECK(w == ref.width && h == ref.height);
        std::vector<uint32_t> buf(size_t(w) * h);
        p.renderPreview32(*raw, buf.data(), w, false);
        int maxDiff = 0;
        for (int y = 2; y < h - 2; ++y)
            for (int x = 2; x < w - 2; ++x) {
                const uint32_t px = buf[size_t(y) * w + x];
                const uint8_t *r = ref.row(y) + x * 3;
                maxDiff = std::max({maxDiff, std::abs(int((px >> 16) & 0xFF) - r[0]), std::abs(int((px >> 8) & 0xFF) - r[1]),
                                    std::abs(int(px & 0xFF) - r[2])});
            }
        CHECK(maxDiff <= 2);
        if (maxDiff > 2)
            std::printf("  variant %d max diff %d\n", variant, maxDiff);
    }
}

static void testStains()
{
    std::printf("IHC colour deconvolution\n");
    // synthetic slide: white background, haematoxylin block, DAB block (Beer-Lambert, sRGB encoded)
    const int w = 300, h = 100;
    Image16 img(w, h);
    const double hv[3] = {0.650, 0.704, 0.286}, dv[3] = {0.268, 0.570, 0.776};
    auto encode = [](double lin) {
        const double s = lin <= 0.0031308 ? 12.92 * lin : 1.055 * std::pow(lin, 1 / 2.4) - 0.055;
        return uint16_t(std::clamp(s, 0.0, 1.0) * 65535.0 + 0.5);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double ch = 0, cd = 0;
            if (x >= 100 && x < 200) ch = 0.8; // haematoxylin only
            if (x >= 200) cd = 0.7;            // DAB only (strong)
            for (int c = 0; c < 3; ++c) {
                const double od = ch * hv[c] / std::sqrt(0.65 * 0.65 + 0.704 * 0.704 + 0.286 * 0.286)
                                  + cd * dv[c] / std::sqrt(0.268 * 0.268 + 0.57 * 0.57 + 0.776 * 0.776);
                img.row(y)[x * 3 + c] = encode(std::pow(10.0, -od));
            }
        }
    StainOptions opt;
    opt.umPerPixel = 0.5;
    StainResult r = analyzeStains(img, opt);
    // tissue = the two stained blocks, DAB positive = the DAB block only
    CHECK_NEAR(double(r.tissuePixels), 200.0 * h, 200);
    CHECK_NEAR(r.positiveFraction, 0.5, 0.02);
    CHECK_NEAR(r.dab[size_t(50) * w + 150], 0.0, 0.05);  // no DAB in the haematoxylin block
    CHECK_NEAR(r.dab[size_t(50) * w + 250], 0.7, 0.05);  // recovered DAB concentration
    CHECK_NEAR(r.h[size_t(50) * w + 150], 0.8, 0.06);
    CHECK_NEAR(r.strong, 0.5, 0.02);
    CHECK_NEAR(r.hScore, 150.0, 3.0);
    CHECK_NEAR(r.positiveAreaUm2, 100.0 * h * 0.25, 60);
    // region restriction: only the DAB block
    StainResult rr = analyzeStains(img, opt, [](int x, int) { return x >= 220; });
    CHECK_NEAR(rr.positiveFraction, 1.0, 0.01);
    // background estimate: this slide has bare glass; a field full of tissue does not
    CHECK(!r.lowBackground());
    {
        Image16 dense(120, 80);
        for (int y = 0; y < dense.height; ++y)
            for (int x = 0; x < dense.width; ++x)
                for (int c = 0; c < 3; ++c)
                    dense.row(y)[x * 3 + c] = encode(std::pow(10.0, -0.4 * hv[c] / std::sqrt(0.65 * 0.65 + 0.704 * 0.704 + 0.286 * 0.286)));
        CHECK(analyzeStains(dense, opt).lowBackground());
    }

    // blank glass with isolated noisy pixels: not tissue when denoising
    {
        Image16 glass(200, 150);
        std::mt19937 grng(3);
        std::uniform_real_distribution<double> gu(0.0, 1.0);
        for (int y = 0; y < glass.height; ++y)
            for (int x = 0; x < glass.width; ++x) {
                const double od = gu(grng) < 0.02 ? 0.25 : 0.0; // 2 % hot/dark single pixels
                for (int c = 0; c < 3; ++c)
                    glass.row(y)[x * 3 + c] = encode(std::pow(10.0, -od * dv[c]));
            }
        StainOptions gopt;
        const StainResult clean = analyzeStains(glass, gopt);
        gopt.denoise = false;
        const StainResult noisy = analyzeStains(glass, gopt);
        std::printf("  blank glass with 2%% noisy pixels: %llu tissue pixels (raw classification %llu)\n",
                    (unsigned long long)clean.tissuePixels, (unsigned long long)noisy.tissuePixels);
        CHECK(clean.tissuePixels < 30);
        CHECK(noisy.tissuePixels > 300);
    }

    // stain vector estimation: a slide whose stains differ from the textbook
    // vectors (bluer haematoxylin, redder DAB), mixtures of both plus background
    {
        const double th[3] = {0.55, 0.75, 0.37}, td[3] = {0.36, 0.60, 0.71};
        auto norm = [](const double v[3], double o[3]) {
            const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            for (int c = 0; c < 3; ++c)
                o[c] = v[c] / n;
        };
        double nh[3], nd[3];
        norm(th, nh);
        norm(td, nd);
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        Image16 slide(400, 300);
        for (int y = 0; y < slide.height; ++y)
            for (int x = 0; x < slide.width; ++x) {
                double ch = 0, cd = 0;
                const double k = u(rng);
                if (k < 0.25) {
                    ch = 0.2 + 0.8 * u(rng); // nuclei
                } else if (k < 0.45) {
                    cd = 0.2 + 0.8 * u(rng); // DAB
                } else if (k < 0.8) {
                    ch = 0.6 * u(rng);       // mixed
                    cd = 0.6 * u(rng);
                }                            // else background
                for (int c = 0; c < 3; ++c)
                    slide.row(y)[x * 3 + c] = encode(std::pow(10.0, -(ch * nh[c] + cd * nd[c])));
            }
        StainVectors est;
        std::string msg;
        const bool ok = estimateStainVectors(slide, est, true, &msg);
        auto angle = [](const double a[3], const double b[3]) {
            const double d = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2])
                             / std::sqrt((a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) * (b[0] * b[0] + b[1] * b[1] + b[2] * b[2]));
            return std::acos(std::clamp(d, -1.0, 1.0)) * 180.0 / 3.14159265358979;
        };
        std::printf("  stain estimation: %s; H %.3f %.3f %.3f (%.1f deg off), DAB %.3f %.3f %.3f (%.1f deg off)\n",
                    msg.c_str(), est.h[0], est.h[1], est.h[2], angle(est.h, nh), est.dab[0], est.dab[1], est.dab[2],
                    angle(est.dab, nd));
        CHECK(ok);
        CHECK(angle(est.h, nh) < 3.0);
        CHECK(angle(est.dab, nd) < 3.0);
        // one stain only: must refuse
        Image16 mono(200, 100);
        for (int y = 0; y < mono.height; ++y)
            for (int x = 0; x < mono.width; ++x) {
                const double conc = x < 100 ? 0.0 : 0.3 + 0.6 * u(rng);
                for (int c = 0; c < 3; ++c)
                    mono.row(y)[x * 3 + c] = encode(std::pow(10.0, -conc * nh[c]));
            }
        const bool monoOk = estimateStainVectors(mono, est, true, &msg);
        std::printf("  single stain: %s -> H %.3f %.3f %.3f, DAB %.3f %.3f %.3f\n", msg.c_str(), est.h[0], est.h[1], est.h[2],
                    est.dab[0], est.dab[1], est.dab[2]);
        CHECK(!monoOk);
    }
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

    // preview() samples the canvas directly: it must equal result() point-sampled
    // every f-th pixel, and stay correct when served from its cache
    for (int maxSize : {100, 237, 4000}) {
        for (int pass = 0; pass < 2; ++pass) {
            double scale = 0;
            Image16 pv = mb.preview(maxSize, scale);
            const int f = std::max(1, (std::max(res.width, res.height) + maxSize - 1) / maxSize);
            CHECK_NEAR(scale, 1.0 / f, 1e-12);
            CHECK(pv.width == res.width / f && pv.height == res.height / f);
            int maxDiff = 0;
            if (pv.width == res.width / f && pv.height == res.height / f)
                for (int y = 0; y < pv.height; ++y)
                    for (int x = 0; x < pv.width; ++x)
                        for (int c = 0; c < 3; ++c)
                            maxDiff = std::max(maxDiff, std::abs(int(pv.row(y)[x * 3 + c]) - int(res.row(y * f)[size_t(x) * f * 3 + c])));
            CHECK(maxDiff == 0);
        }
    }
    // a new tile invalidates the cached preview
    {
        double s0 = 0, s1 = 0;
        Image16 before = mb.preview(4000, s0);
        mb.feed(frameAt(240, 200), true);
        Image16 after = mb.preview(4000, s1);
        Image16 res2 = mb.result();
        CHECK(after.width == res2.width && after.height == res2.height && after.px == res2.px);
        CHECK(mb.status().tiles == st.tiles + 1);
        CHECK(after.px != before.px);
    }
    mb.reset();
    double sEmpty = 0;
    CHECK(mb.preview(100, sEmpty).empty());
}

// Scalar reference of the 3x3 unsharp mask in unsharpMask32 (edge pixels replicated).
static std::vector<uint32_t> unsharp3x3Reference(const std::vector<uint32_t> &src, int w, int h, int stride, double amount)
{
    std::vector<uint32_t> out = src;
    const int a = int(amount * 256);
    auto at = [&](int x, int y, int sh) {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return int((src[size_t(y) * stride + x] >> sh) & 0xFF);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t res = src[size_t(y) * stride + x] & 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                int sum = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        sum += at(x + dx, y + dy, sh);
                const int o = at(x, y, sh) * 9;
                res |= uint32_t(std::clamp((o + ((o - sum) * a >> 8)) / 9, 0, 255)) << sh;
            }
            out[size_t(y) * stride + x] = res;
        }
    return out;
}

static void testUnsharp32()
{
    std::printf("unsharpMask32 (3x3) == scalar reference\n");
    std::mt19937 rng(1234);
    const int sizes[][2] = {{4, 4}, {37, 5}, {64, 17}, {257, 203}, {1920, 1200}};
    for (const auto &sz : sizes) {
        const int w = sz[0], h = sz[1], stride = w + 3;
        std::vector<uint32_t> img(size_t(stride) * h);
        for (auto &v : img)
            v = rng();
        for (double amount : {0.5, 1.7}) {
            const std::vector<uint32_t> ref = unsharp3x3Reference(img, w, h, stride, amount);
            for (int rep = 0; rep < 3; ++rep) {
                std::vector<uint32_t> out = img;
                unsharpMask32(out.data(), w, h, stride, amount, 1.0);
                CHECK(out == ref);
            }
        }
    }
}

// Degenerate 1xN / Nx1 frames must render without reading outside the frame.
static void testThinFrames()
{
    std::printf("1-pixel wide/high frames\n");
    const double g[3] = {0.8, 0.6, 0.4};
    const int dims[][2] = {{1, 1}, {1, 9}, {9, 1}, {1, 2}, {2, 1}, {2, 2}, {3, 1}};
    for (const auto &d : dims) {
        auto raw = makeBayer(d[0], d[1], PixelFormat::BayerGB16, g);
        for (int rot : {0, 90}) {
            ColorSettings s;
            s.rotation = rot;
            ColorPipeline p;
            p.update(s);
            for (bool half : {false, true}) {
                // full-size buffer: renderPreviewHalf32 falls back to full size for tiny frames
                int w, h;
                ColorPipeline::previewSize(*raw, rot, w, h, false);
                std::vector<uint32_t> buf(size_t(w) * h, 0);
                if (half)
                    p.renderPreviewHalf32(*raw, buf.data(), w, false);
                else
                    p.renderPreview32(*raw, buf.data(), w, false);
                CHECK(buf[0] >> 24 == 0xFF);
            }
        }
    }
}

// Focus on YUYV must use luma only: flat luma with textured chroma gives ~0.
static void testFocusYuyv()
{
    std::printf("focus measure (YUYV uses luma)\n");
    auto make = [](bool lumaTexture) {
        RawFrame f;
        f.width = 320;
        f.height = 240;
        f.format = PixelFormat::YUYV;
        f.bitDepth = 8;
        f.stride = f.width * 2;
        f.data.resize(size_t(f.stride) * f.height);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) {
                const uint8_t tex = uint8_t(40 + 180 * scene(x * 1.7, y * 1.7));
                f.data[size_t(y) * f.stride + x * 2] = lumaTexture ? tex : 128;
                f.data[size_t(y) * f.stride + x * 2 + 1] = lumaTexture ? 128 : tex;
            }
        return f;
    };
    const double flatLuma = focusMeasureRaw(make(false));
    const double sharpLuma = focusMeasureRaw(make(true));
    std::printf("  flat luma %.4f, textured luma %.4f\n", flatLuma, sharpLuma);
    CHECK(flatLuma < 1e-6);
    CHECK(sharpLuma > 1.0);
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

static void testNuclei()
{
    std::printf("nucleus detection\n");
    // synthetic field: 48 nuclei (radius 12 px) on a jittered grid, every third DAB positive,
    // two touching pairs, pale cytoplasm, shot noise
    const int W = 640, H = 480, R = 12;
    const double hv[3] = {0.650, 0.704, 0.286}, dv[3] = {0.268, 0.570, 0.776};
    const double hn = std::sqrt(0.65 * 0.65 + 0.704 * 0.704 + 0.286 * 0.286);
    const double dn = std::sqrt(0.268 * 0.268 + 0.57 * 0.57 + 0.776 * 0.776);
    struct Disk {
        double x, y;
        bool pos;
    };
    std::vector<Disk> disks;
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> jit(-8, 8);
    int k = 0;
    for (int gy = 0; gy < 6; ++gy)
        for (int gx = 0; gx < 8; ++gx, ++k)
            disks.push_back({50 + gx * 78 + jit(rng), 45 + gy * 78 + jit(rng), k % 3 == 0});
    // touching pairs (centres 2R apart)
    disks[9].x = disks[8].x + 2 * R + 1;
    disks[9].y = disks[8].y;
    disks[30].x = disks[29].x;
    disks[30].y = disks[29].y + 2 * R + 1;
    int positives = 0;
    for (const auto &d : disks)
        positives += d.pos;
    Image16 img(W, H);
    std::normal_distribution<double> noise(0, 0.01);
    auto encode = [](double lin) {
        const double s = lin <= 0.0031308 ? 12.92 * lin : 1.055 * std::pow(lin, 1 / 2.4) - 0.055;
        return uint16_t(std::clamp(s, 0.0, 1.0) * 65535.0 + 0.5);
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double ch = x > 20 ? 0.06 : 0.0, cd = 0; // pale cytoplasm, a strip of glass
            for (const auto &d : disks) {
                const double r2 = (x - d.x) * (x - d.x) + (y - d.y) * (y - d.y);
                if (r2 <= R * R) {
                    ch = d.pos ? 0.25 : 0.7;
                    cd = d.pos ? 0.6 : 0.0;
                }
            }
            for (int c = 0; c < 3; ++c) {
                const double od = ch * hv[c] / hn + cd * dv[c] / dn + noise(rng);
                img.row(y)[x * 3 + c] = encode(std::pow(10.0, -std::max(0.0, od)));
            }
        }
    StainOptions so;
    const StainResult st = analyzeStains(img, so);
    NucleusOptions no;
    no.diameterPx = 2 * R;
    const NucleusResult nr = detectNuclei(st, no);
    std::printf("  %zu nuclei found (%d expected), %d positive (%d expected), labelling index %.1f %%\n",
                nr.nuclei.size(), int(disks.size()), nr.positive, positives, nr.labellingIndex * 100);
    CHECK_NEAR(double(nr.nuclei.size()), double(disks.size()), 1.0);
    CHECK_NEAR(double(nr.positive), double(positives), 1.0);
    // every detection lies on a real nucleus
    int onNucleus = 0;
    for (const auto &n : nr.nuclei)
        for (const auto &d : disks)
            if (std::hypot(n.x - d.x, n.y - d.y) < R * 0.7) {
                ++onNucleus;
                break;
            }
    CHECK(onNucleus == int(nr.nuclei.size()));

    // cytoplasmic marker: H nuclei, DAB only in the cytoplasm around every third nucleus
    Image16 cyto(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double ch = 0.06, cd = 0;
            for (const auto &d : disks) {
                const double r = std::hypot(x - d.x, y - d.y);
                if (r <= R)
                    ch = 0.7;
                else if (d.pos && r <= 2.2 * R && cd == 0)
                    cd = 0.5;
            }
            for (int c = 0; c < 3; ++c) {
                const double od = ch * hv[c] / hn + cd * dv[c] / dn + noise(rng);
                cyto.row(y)[x * 3 + c] = encode(std::pow(10.0, -std::max(0.0, od)));
            }
        }
    const StainResult cst = analyzeStains(cyto, so);
    NucleusOptions co = no;
    co.nuclearMarker = false;
    const NucleusResult cr = detectNuclei(cst, co);
    std::printf("  cytoplasmic marker: %zu cells (%d expected), %d positive (%d expected)\n", cr.nuclei.size(),
                int(disks.size()), cr.positive, positives);
    CHECK_NEAR(double(cr.nuclei.size()), double(disks.size()), 1.0);
    // touching neighbours share DAB rings, so allow a little spill-over
    CHECK_NEAR(double(cr.positive), double(positives), 3.0);
}

static void testFocusMeasure()
{
    std::printf("focus measure\n");
    // Bayer frame of the texture, optionally blurred (defocus), at a given exposure
    // with shot noise (1 electron per DN)
    const int W = 480, H = 320;
    auto frame = [&](int blur, double exposure, unsigned seed) {
        auto f = std::make_shared<RawFrame>();
        f->width = W;
        f->height = H;
        f->format = PixelFormat::BayerGB16;
        f->bitDepth = 12;
        f->stride = W * 2;
        f->data.resize(size_t(W) * H * 2);
        auto *p = reinterpret_cast<uint16_t *>(f->data.data());
        std::mt19937 rng(seed);
        std::normal_distribution<double> gauss(0.0, 1.0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                double v = 0;
                int n = 0;
                for (int j = -blur; j <= blur; ++j)
                    for (int i = -blur; i <= blur; ++i, ++n)
                        v += scene(x + i, y + j);
                const double dn = v / n * exposure * 4095.0;
                p[size_t(y) * W + x] = uint16_t(std::clamp(dn + std::sqrt(dn) * gauss(rng), 0.0, 4095.0));
            }
        return f;
    };
    const double sharpBright = focusMeasureRaw(*frame(0, 0.7, 1));
    const double sharpDark = focusMeasureRaw(*frame(0, 0.05, 2));
    const double blurBright = focusMeasureRaw(*frame(3, 0.7, 3));
    const double blurDark = focusMeasureRaw(*frame(3, 0.05, 4));
    std::printf("  sharp %.2f (dark %.2f), defocused %.2f (dark %.2f)\n", sharpBright, sharpDark, blurBright, blurDark);
    // independent of exposure: a dark frame must not read as sharper because of noise
    CHECK_NEAR(sharpDark / sharpBright, 1.0, 0.15);
    CHECK_NEAR(blurDark / blurBright, 1.0, 0.2);
    CHECK(sharpBright > 2.0 * blurBright);
    CHECK(sharpDark > 2.0 * blurDark);
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
    // 16- and 36-shot modes: every shot present, same brightness, sensor actually moved
    for (int mi = 1; mi < int(cam->shotModes().size()); ++mi) {
        const auto mode = cam->shotModes()[size_t(mi)];
        std::vector<RawFramePtr> ms;
        const auto tc = std::chrono::steady_clock::now();
        const bool okm = cam->captureShots(mi, ms, err);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - tc).count();
        std::printf("  %s: %s (%zu shots, %.1f s) %s\n", mode.name.c_str(), okm ? "ok" : "FAILED", ms.size(), sec,
                    err.c_str());
        CHECK(okm && int(ms.size()) == mode.shots);
        if (!okm || int(ms.size()) != mode.shots)
            continue;
        double mn = 1e9, mx = 0;
        int moved = 0;
        for (size_t k = 0; k < ms.size(); ++k) {
            const double m = exposureStats(*ms[k], 16).meanLevel;
            mn = std::min(mn, m);
            mx = std::max(mx, m);
            if (k > 0 && ms[k]->data != ms[k - 1]->data)
                ++moved;
        }
        std::printf("    shot mean level %.3f..%.3f, %d/%zu consecutive shots differ\n", mn, mx, moved, ms.size() - 1);
        CHECK(mn > 0.02 && mx / mn < 1.1);
        CHECK(moved == int(ms.size()) - 1);
        Image16 rec = reconstructPixelShift(ms, mode.offsets, {mode.upscale, -1, -1});
        std::printf("    reconstruction %dx%d\n", rec.width, rec.height);
        CHECK(rec.width == ms[0]->width * mode.upscale && rec.height == ms[0]->height * mode.upscale);
    }
    cam->close();
}

static void bench()
{
    const double g[3] = {0.9, 0.7, 0.5};
    auto raw = makeBayer(1920, 1200, PixelFormat::BayerGB16, g);
    ColorPipeline p;
    ColorSettings s;
    s.wbRed = 1.2;
    s.saturation = 1.2;
    p.update(s);
    auto time = [](const char *name, auto &&fn) {
        const int n = 10;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i)
            fn();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("  %-28s %7.2f ms\n", name, ms);
    };
    Image16 lin;
    time("demosaic bilinear", [&] { lin = toLinearRGB(*raw, DemosaicMethod::Bilinear); });
    time("demosaic MHC", [&] { lin = toLinearRGB(*raw, DemosaicMethod::MalvarHeCutler); });
    time("applyLinear", [&] { Image16 t = lin; p.applyLinear(t); });
    time("copy Image16", [&] { Image16 t = lin; (void)t; });
    Image8 d;
    time("toDisplay8", [&] { d = p.toDisplay8(lin); });
    time("histogram", [&] { auto h = computeHistogram(d, 3); (void)h; });
    time("focusMeasure", [&] { (void)focusMeasure(lin); });
    time("exposureStats", [&] { (void)exposureStats(*raw, 8); });
    time("geometry (none)", [&] { auto t = applyGeometry(lin, false, false, 0); (void)t; });
    std::vector<uint32_t> buf(size_t(1920) * 1200);
    time("fused preview32", [&] { p.renderPreview32(*raw, buf.data(), 1920, false); });
    time("half preview32", [&] { p.renderPreviewHalf32(*raw, buf.data(), 960, false); });
    time("unsharpMask32", [&] { unsharpMask32(buf.data(), 1920, 1200, 1920, 0.5, 1.0); });
    time("histogram32", [&] { auto h = computeHistogram32(buf.data(), 1920, 1200, 1920, 3); (void)h; });
    volatile double f = 0;
    time("focusMeasureRaw", [&] { f = f + focusMeasureRaw(*raw); });
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
        bench();
        return 0;
    }
    const bool hw = argc > 1 && std::strcmp(argv[1], "--hw") == 0;
    const bool only = argc > 2 && std::strcmp(argv[2], "--only") == 0;
    if (!only) {
        testDemosaic();
        testPipeline();
        testFastPreview();
        testStains();
        testRegistration();
        testShading();
        testFocusStack();
        testMosaic();
        testUnsharp32();
        testThinFrames();
        testFocusYuyv();
        testPixelShift();
        testSimCamera();
        testFocusMeasure();
        testNuclei();
    }
    if (hw)
        testHardware();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
