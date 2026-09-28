// Integration test of the acquisition engine with the simulated camera:
// live streaming, capture, Multifocus (EDF) and the live image builder.
#include "app/AcquisitionEngine.h"
#include "imaging/Analysis.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <functional>

using namespace lm;

static int g_failed = 0;
#define CHECK(c)                                                                      \
    do {                                                                              \
        if (!(c)) {                                                                   \
            ++g_failed;                                                               \
            std::printf("  FAILED line %d: %s\n", __LINE__, #c);                      \
        }                                                                             \
    } while (0)

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// spins until done() or maxMs; the simulator advances per frame, so tests wait
// for frames or distance rather than a fixed time (slow or busy machines)
static void spinUntil(const std::function<bool()> &done, int maxMs, int stepMs = 50)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < maxMs)
        spin(stepMs);
}

static std::shared_ptr<CaptureResult> waitCapture(AcquisitionEngine &e, int timeoutMs,
                                                  const std::function<void()> &trigger)
{
    std::shared_ptr<CaptureResult> res;
    QEventLoop loop;
    auto c1 = QObject::connect(&e, &AcquisitionEngine::captureFinished, &loop, [&](std::shared_ptr<CaptureResult> r) {
        res = r;
        loop.quit();
    });
    auto c2 = QObject::connect(&e, &AcquisitionEngine::captureFailed, &loop, [&](const QString &m) {
        std::printf("  capture failed: %s\n", qPrintable(m));
        loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    trigger(); // may emit synchronously
    if (!res)
        loop.exec();
    QObject::disconnect(c1);
    QObject::disconnect(c2);
    return res;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    AcquisitionEngine e;
    int frames = 0;
    QObject::connect(&e, &AcquisitionEngine::frameReady, &app, [&](const QImage &, const LiveStats &) {
        ++frames;
        e.frameConsumed();
    });

    CameraInfo sim;
    for (const auto &c : e.enumerateCameras())
        if (c.backend == "Simulator")
            sim = c;
    QString err;
    std::printf("open simulator\n");
    CHECK(e.openCamera(sim, err));
    Camera *cam = e.camera();
    CHECK(cam != nullptr);
    if (!cam)
        return 1;
    cam->setResolutionIndex(1); // 1024 x 768 for speed
    cam->setExposure(10);
    ColorSettings cs;
    cs.wbRed = 1.0;
    cs.wbBlue = 1.8;
    e.setColorSettings(cs);
    CHECK(e.startLive(err));
    // wait for frames rather than for a fixed time: a slow or busy machine (a CI
    // runner, an emulated build) displays fewer per second
    spinUntil([&] { return frames > 10; }, 15000);
    std::printf("  live frames displayed: %d\n", frames);
    CHECK(frames > 10);

    // --- white balance request
    {
        double r = 0, b = 0;
        QEventLoop loop;
        QObject::connect(&e, &AcquisitionEngine::whiteBalanceComputed, &loop, [&](double rr, double, double bb) {
            r = rr;
            b = bb;
            loop.quit();
        });
        e.requestWhiteBalance();
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        std::printf("white balance: R %.3f B %.3f\n", r, b);
        // simulator lamp R 1.0, G 0.82, B 0.55 -> ideal gains R 0.82, B 1.49
        CHECK(r > 0.7 && r < 0.95 && b > 1.25 && b < 1.75);
    }

    // --- single capture with averaging
    std::printf("capture (4 frames averaged)\n");
    auto res = waitCapture(e, 10000, [&] { e.capture(4); });
    CHECK(res && res->rendered16.width == 1024 && res->averagedFrames == 4);

    // --- HDR: three exposures merged; the exposure is restored afterwards
    std::printf("HDR capture (3 exposures)\n");
    {
        const double before = cam->exposure();
        auto hdr = waitCapture(e, 20000, [&] { e.captureHdr(3, 1); });
        CHECK(hdr && hdr->kind == "hdr-3" && hdr->rendered16.width == 1024);
        CHECK(std::abs(cam->exposure() - before) < 0.01);
        spin(300);
        CHECK(!e.isBusy());
    }

    // --- multifocus: sweep focus through the section, compare sharpness
    std::printf("multifocus\n");
    cam->setProperty("tilt", 12.0);
    cam->setProperty("focus", 0.0);
    spin(300);
    auto single = waitCapture(e, 10000, [&] { e.capture(1); });
    e.setLiveMode(LiveMode::Multifocus);
    for (double f = -7; f <= 7; f += 1.0) {
        cam->setProperty("focus", f);
        // at least 120 ms and two new frames per focus step (slow machines deliver fewer)
        const uint64_t n0 = e.framesReceived();
        spin(120);
        spinUntil([&] { return e.framesReceived() >= n0 + 2; }, 1400, 20);
    }
    std::printf("  frames merged: %d\n", e.focusStacker().frameCount());
    CHECK(e.focusStacker().frameCount() >= 8);
    auto edf = waitCapture(e, 10000, [&] { e.finishMultifocus(); });
    CHECK(edf && edf->kind == "multifocus");
    if (edf && single) {
        // sharpness in the outer bands (defocused in the single frame)
        auto bandFocus = [](const Image16 &img, int band) {
            const int w = img.width / 6;
            return focusMeasure(img, {band * w + 8, img.height / 4, w - 16, img.height / 2});
        };
        const double s0 = bandFocus(single->linear, 0), e0 = bandFocus(edf->linear, 0);
        const double s5 = bandFocus(single->linear, 5), e5 = bandFocus(edf->linear, 5);
        std::printf("  edge band sharpness single %.2f / %.2f  ->  EDF %.2f / %.2f\n", s0, s5, e0, e5);
        CHECK(e0 > s0 * 1.5 && e5 > s5 * 1.5);
    }
    cam->setProperty("tilt", 0.0);
    cam->setProperty("focus", 0.0);

    // --- live image builder: drift the stage and stitch
    std::printf("live image builder\n");
    e.setLiveMode(LiveMode::Mosaic);
    spin(500);
    // slow continuous scanning (5 sensor px / frame), as a user would move the stage.
    // The drift is per frame, so each leg runs until the stage has covered its
    // distance rather than for a fixed time: a slow or busy machine delivers fewer
    // frames per second and would otherwise build a smaller mosaic.
    auto stage = [&](const char *key) {
        for (const auto &p : cam->properties())
            if (p.key == key)
                return p.value;
        return 0.0;
    };
    const double x0 = stage("stage_x"), y0 = stage("stage_y");
    cam->setProperty("drift_x", 5.0);
    spinUntil([&] { return stage("stage_x") >= x0 + 600; }, 20000);
    cam->setProperty("drift_x", 0.0);
    cam->setProperty("drift_y", 5.0);
    spinUntil([&] { return stage("stage_y") >= y0 + 450; }, 20000);
    cam->setProperty("drift_y", 0.0);
    spinUntil([&] { return e.mosaic().status().tiles >= 3; }, 10000); // at rest -> last tile added
    spin(300);
    const auto st = e.mosaic().status();
    std::printf("  tiles %d tracking %d\n", st.tiles, int(st.tracking));
    CHECK(st.tiles >= 3);
    auto mos = waitCapture(e, 20000, [&] { e.finishMosaic(); });
    CHECK(mos && mos->kind == "mosaic");
    if (mos) {
        std::printf("  mosaic size %d x %d (frame 1024 x 768)\n", mos->linear.width, mos->linear.height);
        CHECK(mos->linear.width > 1024 + 200);
        CHECK(mos->linear.height > 768 + 100);
        if (qEnvironmentVariableIsSet("SAVE_RESULTS")) {
            for (auto [img, name] : {std::pair{&mos->rendered8, "mosaic.ppm"}, std::pair{edf ? &edf->rendered8 : nullptr, "edf.ppm"},
                                     std::pair{single ? &single->rendered8 : nullptr, "single.ppm"}}) {
                if (!img)
                    continue;
                if (FILE *f = std::fopen(name, "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
                    std::fwrite(img->px.data(), 1, img->px.size(), f);
                    std::fclose(f);
                }
            }
        }
    }

    e.stopLive();
    e.closeCamera();
    std::printf("\n%s (%d failures)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
