#pragma once
// Central acquisition engine: owns the active camera, runs the live processing
// thread (demosaic -> colour pipeline -> display image + statistics), the auto
// exposure loop, captures, focus stacking and live stitching.

#include "camera/Camera.h"
#include "imaging/Analysis.h"
#include "imaging/ColorPipeline.h"
#include "imaging/StainAnalysis.h"
#include "imaging/FocusStacker.h"
#include "imaging/MosaicBuilder.h"
#include "imaging/ShadingCorrection.h"

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QThreadPool>
#include <QWaitCondition>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>

namespace lm {

struct LiveStats {
    Histogram histogram;       // of the displayed image
    double focus = 0.0;        // focus measure (higher = sharper)
    double fps = 0.0;
    double displayFps = 0.0;
    double meanLevel = 0.0;    // raw exposure level 0..1
    double saturated = 0.0;    // fraction of saturated raw pixels
    int width = 0, height = 0;
    uint64_t frames = 0;
    uint64_t dropped = 0;
    double displayScale = 1.0; // displayed pixels per sensor pixel (mosaic preview < 1)
    // live IHC analysis (when enabled): overlay of the displayed image size, DAB-positive % of tissue
    QImage dabOverlay;
    double dabPositive = -1.0;
};

enum class LiveMode { Normal, Multifocus, Mosaic };

struct AutoExposureSettings {
    bool enabled = false;
    double target = 0.85;      // target 99th percentile level (linear 0..1): bright field background just below white
    double maxExposureMs = 500;
    bool allowGain = false;
};

// A captured result delivered to the UI
struct CaptureResult {
    Image16 linear;            // linear, after shading/WB/colour (pre tone curve)
    Image16 rendered16;        // tone mapped 16-bit
    Image8 rendered8;          // tone mapped 8-bit
    double exposureMs = 0;
    double gain = 1;
    int averagedFrames = 1;
    int upscale = 1;           // output pixels per sensor pixel (pixel shift)
    std::vector<double> exposureSeriesMs; // HDR: the merged exposures
    std::string kind;          // "single", "multifocus", "mosaic", "pixelshift-N"
};

class AcquisitionEngine : public QObject {
    Q_OBJECT
public:
    explicit AcquisitionEngine(QObject *parent = nullptr);
    ~AcquisitionEngine() override;

    // camera management
    std::vector<CameraInfo> enumerateCameras();
    bool openCamera(const CameraInfo &info, QString &error);
    void closeCamera();
    Camera *camera() const { return m_camera.get(); } // UI thread only
    // Waits for background capture/reconstruction jobs (used before shutdown).
    void waitForJobs();
    bool isLive() const;
    bool startLive(QString &error);
    void stopLive();
    void setFrozen(bool frozen) { m_frozen = frozen; }
    bool isFrozen() const { return m_frozen; }

    // processing
    void setColorSettings(const ColorSettings &s);
    ColorSettings colorSettings() const;
    void setShading(std::shared_ptr<const ShadingCorrection> sc);
    std::shared_ptr<const ShadingCorrection> shading() const;
    void setShadingEnabled(bool on);
    bool shadingEnabled() const { return m_shadingEnabled; }
    void setAutoExposure(const AutoExposureSettings &s);
    AutoExposureSettings autoExposure() const;
    void setShowClipping(bool on) { m_showClipping = on; }
    // live DAB overlay (IHC), refreshed a few times per second
    void setLiveDab(bool on, const StainOptions &opt);
    void setPreviewQuality(bool high) { m_previewHighQuality = high; }
    // half resolution live preview (view zoomed out)
    void setPreviewHalf(bool half) { m_previewHalf = half; }
    // false while nobody looks at the live image (other workspace, minimised):
    // the live preview is then rendered about once a second to save CPU
    void setPreviewVisible(bool visible) { m_previewVisible = visible; }
    void setFocusRegion(Rect r) { QMutexLocker l(&m_mutex); m_focusRegion = r; }

    // one-shot operations executed on the next live frame
    void requestWhiteBalance(Rect region = {});
    void requestBlackBalance();
    void requestShadingReference(int frames = 8);
    void requestAutoExposureOnce();
    void requestAutoLevels();

    // capture: averages `frames` raw frames, full quality demosaic
    void capture(int averageFrames = 1);
    // multi-shot sensor-shift capture (camera shot mode index)
    void captureShots(int modeIndex);
    // high dynamic range: `exposures` exposures kHdrStops EV apart from the current
    // one upwards, `averageFrames` each, merged in raw (camera must supportsHdr())
    void captureHdr(int exposures, int averageFrames = 1);
    bool isBusy() const { return m_busy; }
    uint64_t framesReceived() const { return m_received; }
    std::shared_ptr<const ColorPipeline> pipeline() const;

    // multifocus / live image builder
    void setLiveMode(LiveMode m);
    LiveMode liveMode() const { return m_mode; }
    FocusStacker &focusStacker() { return m_stacker; }
    MosaicBuilder &mosaic() { return m_mosaic; }
    void mosaicAddTile() { m_mosaicForceAdd = true; }
    void finishMultifocus();
    void finishMosaic();

    // Last raw frame (for pixel readout etc.)
    RawFramePtr lastRaw() const;

    // The UI calls this after it has displayed a frame (back-pressure: the
    // engine renders a new display image only when the previous one was taken).
    void frameConsumed() { m_uiBusy = false; }

    // Abandons a capture that is still waiting for frames (emits captureFailed); thread safe
    void cancelPendingCapture(const QString &reason);

signals:
    void frameReady(const QImage &display, const lm::LiveStats &stats);
    void cameraError(const QString &message);
    // an error while processing a frame (the camera itself is fine)
    void processingError(const QString &message);
    void whiteBalanceComputed(double r, double g, double b);
    void blackLevelComputed(double level);
    void levelsComputed(double blackPoint, double whitePoint);
    void shadingReferenceReady(std::shared_ptr<lm::ShadingCorrection> sc);
    void exposureChanged(double ms, double gain);
    void captureFinished(std::shared_ptr<lm::CaptureResult> result);
    void captureFailed(const QString &message);
    void captureProgress(int done, int total, const QString &what);
    void multifocusProgress(int frames, double improvedFraction);
    void mosaicStatus(const lm::MosaicBuilder::Status &status);
    void liveStateChanged(bool live);

private:
    void onRawFrame(RawFramePtr f);
    void processingLoop();
    void runAutoExposure(const RawFrame &raw, Camera *cam);
    QImage toQImage(const Image8 &img, bool clipping, const Image16 *linear);

    std::vector<std::unique_ptr<CameraBackend>> m_backends;
    // shared: worker threads keep a reference while they use the camera
    std::shared_ptr<Camera> m_camera;
    std::array<double, 9> m_cameraMatrix{1, 0, 0, 0, 1, 0, 0, 0, 1}; // guarded by m_mutex
    std::shared_ptr<Camera> cameraRef() const;
    QThreadPool m_jobs; // capture / reconstruction jobs (joined on shutdown)

    mutable QMutex m_mutex;
    QWaitCondition m_frameCond;
    RawFramePtr m_pending;      // latest unprocessed frame (older ones dropped)
    RawFramePtr m_last;         // last processed raw frame
    std::deque<RawFramePtr> m_captureQueue;
    int m_captureWanted = 0;    // frames still needed for a capture
    int m_captureAverage = 1;
    // frames of a given exposure for a capture job (HDR); guarded by m_mutex
    std::deque<RawFramePtr> m_grabbed;
    int m_grabWanted = 0, m_grabSkip = 0;
    double m_grabExposureMs = 0;
    QWaitCondition m_grabCond;
    // waits (on a job thread) for `n` frames of `cam` exposed at `exposureMs`
    std::deque<RawFramePtr> grabFrames(const std::shared_ptr<Camera> &cam, int n, double exposureMs, int timeoutMs);
    // capture job progress / failure, delivered on the GUI thread
    void postProgress(int done, int total, const QString &what);
    void postFailure(const QString &message);

    ColorSettings m_color;
    std::shared_ptr<const ColorPipeline> m_pipeline; // immutable, swapped on change
    std::shared_ptr<const ShadingCorrection> m_shading;
    std::atomic<bool> m_shadingEnabled{false};
    AutoExposureSettings m_ae;
    std::atomic<bool> m_aeOnce{false};
    std::atomic<bool> m_showClipping{false};
    std::atomic<bool> m_liveDab{false};
    StainOptions m_liveDabOptions;        // guarded by m_mutex
    QImage m_dabOverlay;                  // processing thread only
    double m_dabPositive = -1.0;
    std::chrono::steady_clock::time_point m_lastDab{};
    std::atomic<bool> m_previewHighQuality{false};
    std::atomic<bool> m_previewHalf{false};
    std::atomic<bool> m_previewVisible{true};
    std::atomic<bool> m_frozen{false};
    Rect m_focusRegion;

    std::atomic<bool> m_wbRequest{false};
    Rect m_wbRegion;
    std::atomic<bool> m_blackRequest{false};
    std::atomic<bool> m_levelsRequest{false};
    std::atomic<int> m_shadingFramesWanted{0};
    std::vector<Image16> m_shadingFrames;

    std::atomic<LiveMode> m_mode{LiveMode::Normal};
    FocusStacker m_stacker;
    MosaicBuilder m_mosaic;
    std::atomic<bool> m_mosaicForceAdd{false};

    std::thread m_worker;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_busy{false};
    // held by auto exposure from its m_busy check to its setExposure, so a capture
    // job that set m_busy and then passed this lock owns the exposure
    QMutex m_exposureLock;
    std::atomic<bool> m_uiBusy{false};
    std::chrono::steady_clock::time_point m_uiBusySince{};
    std::atomic<uint64_t> m_received{0}, m_dropped{0};
    double m_fps = 0, m_displayFps = 0;
    std::chrono::steady_clock::time_point m_lastFrameTime{}, m_lastDisplayTime{};
    std::chrono::steady_clock::time_point m_lastAeChange{};
};

} // namespace lm

Q_DECLARE_METATYPE(lm::LiveStats)
Q_DECLARE_METATYPE(lm::MosaicBuilder::Status)
Q_DECLARE_METATYPE(std::shared_ptr<lm::CaptureResult>)
Q_DECLARE_METATYPE(std::shared_ptr<lm::ShadingCorrection>)
