#include "AcquisitionEngine.h"

#include "imaging/Debayer.h"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace lm {

namespace {
using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t)
{
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// Averages several raw frames of identical geometry into one 16-bit frame
// (gaining up to log2(n)/2 bits of precision).
RawFramePtr averageFrames(const std::deque<RawFramePtr> &frames)
{
    if (frames.empty())
        return nullptr;
    if (frames.size() == 1)
        return frames.front();
    const RawFrame &f0 = *frames.front();
    const bool wide = is16Bit(f0.format);
    const int spp = bytesPerPixel(f0.format) / (wide ? 2 : 1); // samples per pixel
    const size_t samples = size_t(f0.width) * f0.height * spp;
    std::vector<uint32_t> acc(samples, 0);
    int n = 0;
    for (const auto &f : frames) {
        if (!f || f->width != f0.width || f->height != f0.height || f->format != f0.format)
            continue;
        for (int y = 0; y < f->height; ++y) {
            const uint8_t *row = f->data.data() + size_t(y) * f->stride;
            uint32_t *a = acc.data() + size_t(y) * f->width * spp;
            if (wide) {
                const uint16_t *r16 = reinterpret_cast<const uint16_t *>(row);
                for (int i = 0; i < f->width * spp; ++i)
                    a[i] += r16[i];
            } else {
                for (int i = 0; i < f->width * spp; ++i)
                    a[i] += row[i];
            }
        }
        ++n;
    }
    auto out = std::make_shared<RawFrame>(f0);
    // promote to 16 bit to keep the extra precision
    PixelFormat fmt = f0.format;
    switch (f0.format) {
    case PixelFormat::Mono8: fmt = PixelFormat::Mono16; break;
    case PixelFormat::BayerRG8: fmt = PixelFormat::BayerRG16; break;
    case PixelFormat::BayerGR8: fmt = PixelFormat::BayerGR16; break;
    case PixelFormat::BayerGB8: fmt = PixelFormat::BayerGB16; break;
    case PixelFormat::BayerBG8: fmt = PixelFormat::BayerBG16; break;
    case PixelFormat::RGB8: fmt = PixelFormat::RGB16; break;
    default: break;
    }
    if (f0.format == PixelFormat::BGR8 || f0.format == PixelFormat::BGRA8 || f0.format == PixelFormat::YUYV
        || f0.format == PixelFormat::NV12) {
        // packed formats that we do not promote: plain average in place
        for (size_t i = 0; i < samples; ++i)
            out->data[i] = uint8_t((acc[i] + n / 2) / n);
        return out;
    }
    const int inBits = f0.bitDepth;
    const int outBits = std::min(16, inBits + 4);
    const double scale = double((1 << outBits) - 1) / ((1 << inBits) - 1) / n;
    out->format = fmt;
    out->bitDepth = outBits;
    out->stride = f0.width * spp * 2;
    out->data.assign(size_t(out->stride) * f0.height, 0);
    uint16_t *o = reinterpret_cast<uint16_t *>(out->data.data());
    for (size_t i = 0; i < samples; ++i)
        o[i] = uint16_t(std::min(65535.0, acc[i] * scale + 0.5));
    return out;
}
} // namespace

AcquisitionEngine::AcquisitionEngine(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<lm::LiveStats>();
    qRegisterMetaType<lm::MosaicBuilder::Status>();
    qRegisterMetaType<std::shared_ptr<lm::CaptureResult>>();
    qRegisterMetaType<std::shared_ptr<lm::ShadingCorrection>>();
    m_backends = createBackends();
    m_pipeline = std::make_shared<ColorPipeline>();
    m_running = true;
    m_worker = std::thread([this] { processingLoop(); });
}

AcquisitionEngine::~AcquisitionEngine()
{
    closeCamera();
    m_running = false;
    m_frameCond.wakeAll();
    if (m_worker.joinable())
        m_worker.join();
}

std::vector<CameraInfo> AcquisitionEngine::enumerateCameras()
{
    std::vector<CameraInfo> all;
    for (auto &b : m_backends) {
        auto list = b->enumerate();
        all.insert(all.end(), list.begin(), list.end());
    }
    return all;
}

bool AcquisitionEngine::openCamera(const CameraInfo &info, QString &error)
{
    closeCamera();
    for (auto &b : m_backends) {
        if (b->name() != info.backend)
            continue;
        auto cam = b->create(info);
        if (!cam)
            continue;
        std::string err;
        if (!cam->open(err)) {
            error = QString::fromStdString(err);
            return false;
        }
        cam->setFrameCallback([this](RawFramePtr f) { onRawFrame(std::move(f)); });
        cam->setErrorCallback([this](const std::string &e) { emit cameraError(QString::fromStdString(e)); });
        m_camera = std::move(cam);
        return true;
    }
    error = tr("No backend for camera %1").arg(QString::fromStdString(info.name));
    return false;
}

void AcquisitionEngine::closeCamera()
{
    if (m_camera) {
        m_camera->stopStreaming();
        m_camera->setFrameCallback(nullptr);
        m_camera->close();
        m_camera.reset();
        emit liveStateChanged(false);
    }
    QMutexLocker l(&m_mutex);
    m_pending.reset();
}

bool AcquisitionEngine::isLive() const
{
    return m_camera && m_camera->isStreaming();
}

bool AcquisitionEngine::startLive(QString &error)
{
    if (!m_camera) {
        error = tr("No camera open");
        return false;
    }
    std::string err;
    if (!m_camera->startStreaming(err)) {
        error = QString::fromStdString(err);
        return false;
    }
    m_lastFrameTime = {};
    emit liveStateChanged(true);
    return true;
}

void AcquisitionEngine::stopLive()
{
    if (m_camera)
        m_camera->stopStreaming();
    emit liveStateChanged(false);
}

void AcquisitionEngine::setColorSettings(const ColorSettings &s)
{
    auto p = std::make_shared<ColorPipeline>();
    p->update(s);
    QMutexLocker l(&m_mutex);
    m_color = s;
    if (m_shadingEnabled)
        p->setShading(m_shading);
    m_pipeline = p;
}

ColorSettings AcquisitionEngine::colorSettings() const
{
    QMutexLocker l(&m_mutex);
    return m_color;
}

void AcquisitionEngine::setShading(std::shared_ptr<const ShadingCorrection> sc)
{
    {
        QMutexLocker l(&m_mutex);
        m_shading = std::move(sc);
    }
    setColorSettings(colorSettings());
}

std::shared_ptr<const ShadingCorrection> AcquisitionEngine::shading() const
{
    QMutexLocker l(&m_mutex);
    return m_shading;
}

void AcquisitionEngine::setShadingEnabled(bool on)
{
    m_shadingEnabled = on;
    setColorSettings(colorSettings());
}

void AcquisitionEngine::setAutoExposure(const AutoExposureSettings &s)
{
    QMutexLocker l(&m_mutex);
    m_ae = s;
}

AutoExposureSettings AcquisitionEngine::autoExposure() const
{
    QMutexLocker l(&m_mutex);
    return m_ae;
}

void AcquisitionEngine::requestWhiteBalance(Rect region)
{
    QMutexLocker l(&m_mutex);
    m_wbRegion = region;
    m_wbRequest = true;
}

void AcquisitionEngine::requestBlackBalance()
{
    m_blackRequest = true;
}

void AcquisitionEngine::requestShadingReference(int frames)
{
    QMutexLocker l(&m_mutex);
    m_shadingFrames.clear();
    m_shadingFramesWanted = std::max(1, frames);
}

void AcquisitionEngine::requestAutoExposureOnce()
{
    m_aeOnce = true;
}

void AcquisitionEngine::setLiveMode(LiveMode m)
{
    if (m == LiveMode::Multifocus)
        m_stacker.reset();
    if (m == LiveMode::Mosaic)
        m_mosaic.reset();
    m_mode = m;
}

RawFramePtr AcquisitionEngine::lastRaw() const
{
    QMutexLocker l(&m_mutex);
    return m_last;
}

void AcquisitionEngine::capture(int averageFrames)
{
    QMutexLocker l(&m_mutex);
    if (!m_camera || !m_camera->isStreaming()) {
        // no live stream: capture from the last frame if we have one
        if (m_last) {
            m_captureQueue.clear();
            m_captureQueue.push_back(m_last);
            m_captureAverage = 1;
            m_captureWanted = 0;
            auto frames = m_captureQueue;
            m_captureQueue.clear();
            l.unlock();
            QMetaObject::invokeMethod(this, [this, frames] {
                // reuse the same path as live captures
                auto pipeline = [this] { QMutexLocker k(&m_mutex); return m_pipeline; }();
                auto res = std::make_shared<CaptureResult>();
                auto raw = averageFrames(frames);
                res->linear = toLinearRGB(*raw, DemosaicMethod::MalvarHeCutler);
                pipeline->applyLinear(res->linear);
                const auto &cs = pipeline->settings();
                res->linear = applyGeometry(res->linear, cs.flipHorizontal, cs.flipVertical, cs.rotation);
                res->rendered16 = pipeline->toDisplay16(res->linear);
                unsharpMask(res->rendered16, cs.sharpenAmount, cs.sharpenRadius);
                res->rendered8 = pipeline->toDisplay8(res->linear);
                unsharpMask(res->rendered8, cs.sharpenAmount, cs.sharpenRadius);
                res->exposureMs = raw->exposureMs;
                res->gain = raw->gain;
                res->kind = "single";
                emit captureFinished(res);
            }, Qt::QueuedConnection);
            return;
        }
        l.unlock();
        emit captureFailed(tr("Camera is not streaming"));
        return;
    }
    m_captureQueue.clear();
    m_captureAverage = std::clamp(averageFrames, 1, 64);
    // skip one frame so the capture starts with a fresh exposure
    m_captureWanted = m_captureAverage + 1;
}

void AcquisitionEngine::onRawFrame(RawFramePtr f)
{
    if (!f || f->empty())
        return;
    const auto now = Clock::now();
    if (m_lastFrameTime.time_since_epoch().count() != 0) {
        const double dt = std::chrono::duration<double>(now - m_lastFrameTime).count();
        if (dt > 0)
            m_fps = m_fps == 0 ? 1.0 / dt : 0.9 * m_fps + 0.1 / dt;
    }
    m_lastFrameTime = now;
    ++m_received;

    std::deque<RawFramePtr> captureFrames;
    std::shared_ptr<const ColorPipeline> pipeline;
    {
        QMutexLocker l(&m_mutex);
        if (m_captureWanted > 0) {
            if (m_captureWanted <= m_captureAverage)
                m_captureQueue.push_back(f);
            if (--m_captureWanted == 0) {
                captureFrames.swap(m_captureQueue);
                pipeline = m_pipeline;
            }
        }
        if (m_pending)
            ++m_dropped;
        m_pending = f;
    }
    m_frameCond.wakeOne();

    if (!captureFrames.empty()) {
        // full quality processing off the camera thread
        QtConcurrent::run([this, captureFrames = std::move(captureFrames), pipeline] {
            try {
                auto raw = averageFrames(captureFrames);
                auto res = std::make_shared<CaptureResult>();
                res->averagedFrames = int(captureFrames.size());
                res->exposureMs = raw->exposureMs;
                res->gain = raw->gain;
                res->kind = "single";
                res->linear = toLinearRGB(*raw, DemosaicMethod::MalvarHeCutler);
                pipeline->applyLinear(res->linear);
                const auto &cs = pipeline->settings();
                res->linear = applyGeometry(res->linear, cs.flipHorizontal, cs.flipVertical, cs.rotation);
                res->rendered16 = pipeline->toDisplay16(res->linear);
                unsharpMask(res->rendered16, cs.sharpenAmount, cs.sharpenRadius);
                res->rendered8 = pipeline->toDisplay8(res->linear);
                unsharpMask(res->rendered8, cs.sharpenAmount, cs.sharpenRadius);
                emit captureFinished(res);
            } catch (const std::exception &e) {
                emit captureFailed(QString::fromUtf8(e.what()));
            }
        });
    }
}

void AcquisitionEngine::runAutoExposure(const RawFrame &raw)
{
    AutoExposureSettings ae;
    {
        QMutexLocker l(&m_mutex);
        ae = m_ae;
    }
    const bool once = m_aeOnce;
    if ((!ae.enabled && !once) || !m_camera)
        return;
    // wait until frames reflect the last change
    if (secondsSince(m_lastAeChange) < 0.15)
        return;
    const double current = m_camera->exposure();
    if (raw.exposureMs > 0 && std::abs(raw.exposureMs - current) > std::max(0.01, current * 0.02))
        return;
    const ExposureStats st = exposureStats(raw);
    const double level = std::max(st.percentile99, 1e-3);
    double factor = ae.target / level;
    if (st.saturatedFraction > 0.02)
        factor = std::min(factor, 0.5);
    factor = std::clamp(factor, 0.2, 5.0);
    if (std::abs(factor - 1.0) < 0.04) {
        m_aeOnce = false;
        return;
    }
    const Range r = m_camera->exposureRange();
    double target = std::clamp(current * factor, r.min, std::min(r.max, ae.maxExposureMs));
    double gain = m_camera->gain();
    if (ae.allowGain) {
        // push gain up only once exposure is at its limit; bring it down first
        if (factor > 1.0 && target >= std::min(r.max, ae.maxExposureMs) - 1e-6) {
            const Range gr = m_camera->gainRange();
            gain = std::clamp(gain * factor * current / std::max(target, 1e-6), gr.min, gr.max);
            m_camera->setGain(gain);
        } else if (factor < 1.0 && gain > 1.0) {
            const Range gr = m_camera->gainRange();
            gain = std::clamp(gain * factor, gr.min, gr.max);
            m_camera->setGain(gain);
            target = current;
        }
    }
    if (std::abs(target - current) > 1e-6)
        m_camera->setExposure(target);
    m_lastAeChange = Clock::now();
    emit exposureChanged(m_camera->exposure(), m_camera->gain());
}

QImage AcquisitionEngine::toQImage(const Image8 &img, bool clipping, const Image16 *linear)
{
    QImage q(img.width, img.height, QImage::Format_RGB888);
    for (int y = 0; y < img.height; ++y) {
        uchar *d = q.scanLine(y);
        std::memcpy(d, img.row(y), size_t(img.width) * 3);
        if (clipping && linear && linear->width == img.width && linear->height == img.height) {
            const uint16_t *l = linear->row(y);
            for (int x = 0; x < img.width; ++x) {
                const uint16_t r = l[x * 3], g = l[x * 3 + 1], b = l[x * 3 + 2];
                if (r >= 65000 || g >= 65000 || b >= 65000) {
                    d[x * 3] = 255; d[x * 3 + 1] = 0; d[x * 3 + 2] = 0;
                } else if (r < 200 && g < 200 && b < 200) {
                    d[x * 3] = 0; d[x * 3 + 1] = 64; d[x * 3 + 2] = 255;
                }
            }
        }
    }
    return q;
}

void AcquisitionEngine::processingLoop()
{
    while (m_running) {
        RawFramePtr raw;
        {
            QMutexLocker l(&m_mutex);
            if (!m_pending)
                m_frameCond.wait(&m_mutex, 100);
            raw = std::move(m_pending);
            m_pending.reset();
            if (raw)
                m_last = raw;
        }
        if (!raw)
            continue;
        try {
            runAutoExposure(*raw);

            std::shared_ptr<const ColorPipeline> pipeline;
            Rect focusRegion, wbRegion;
            {
                QMutexLocker l(&m_mutex);
                pipeline = m_pipeline;
                focusRegion = m_focusRegion;
                wbRegion = m_wbRegion;
            }
            const auto &cs = pipeline->settings();
            const bool hq = m_previewHighQuality;
            Image16 lin = toLinearRGB(*raw, hq ? DemosaicMethod::MalvarHeCutler : DemosaicMethod::Bilinear);

            // --- one shot calibrations (operate on data before WB/colour) ---
            if (m_blackRequest.exchange(false)) {
                const double bl = estimateBlackLevel(lin);
                QMetaObject::invokeMethod(this, [this, bl] { emit blackLevelComputed(bl); }, Qt::QueuedConnection);
            }
            if (m_wbRequest || m_shadingFramesWanted > 0) {
                ColorSettings neutral;
                neutral.blackLevel = cs.blackLevel;
                ColorPipeline pre;
                pre.update(neutral);
                if (m_shadingEnabled)
                    pre.setShading(pipeline->shading());
                if (m_wbRequest.exchange(false)) {
                    Image16 tmp = lin;
                    pre.applyLinear(tmp);
                    auto g = computeWhiteBalance(tmp, wbRegion);
                    QMetaObject::invokeMethod(this, [this, g] { emit whiteBalanceComputed(g[0], g[1], g[2]); },
                                              Qt::QueuedConnection);
                }
                if (m_shadingFramesWanted > 0) {
                    ColorPipeline blackOnly;
                    blackOnly.update(neutral);
                    Image16 tmp = lin;
                    blackOnly.applyLinear(tmp);
                    std::vector<Image16> frames;
                    {
                        QMutexLocker l(&m_mutex);
                        m_shadingFrames.push_back(std::move(tmp));
                        if (int(m_shadingFrames.size()) >= m_shadingFramesWanted) {
                            frames.swap(m_shadingFrames);
                            m_shadingFramesWanted = 0;
                        }
                    }
                    if (!frames.empty()) {
                        Image16 avg(frames[0].width, frames[0].height);
                        std::vector<uint32_t> acc(avg.px.size(), 0);
                        for (auto &f : frames)
                            if (f.px.size() == acc.size())
                                for (size_t i = 0; i < acc.size(); ++i)
                                    acc[i] += f.px[i];
                        for (size_t i = 0; i < acc.size(); ++i)
                            avg.px[i] = uint16_t(acc[i] / frames.size());
                        auto sc = ShadingCorrection::fromReference(avg);
                        QMetaObject::invokeMethod(this, [this, sc] { emit shadingReferenceReady(sc); },
                                                  Qt::QueuedConnection);
                    }
                }
            }

            pipeline->applyLinear(lin);
            lin = applyGeometry(lin, cs.flipHorizontal, cs.flipVertical, cs.rotation);

            LiveStats st;
            st.width = raw->width;
            st.height = raw->height;
            st.frames = m_received;
            st.dropped = m_dropped;
            st.fps = m_fps;
            {
                const ExposureStats es = exposureStats(*raw, 8);
                st.meanLevel = es.meanLevel;
                st.saturated = es.saturatedFraction;
            }
            st.focus = focusMeasure(lin, focusRegion);

            QImage display;
            const LiveMode mode = m_mode;
            if (mode == LiveMode::Multifocus) {
                const double improved = m_stacker.add(lin);
                const int n = m_stacker.frameCount();
                QMetaObject::invokeMethod(this, [this, n, improved] { emit multifocusProgress(n, improved); },
                                          Qt::QueuedConnection);
                Image16 comp = m_stacker.result();
                Image8 out = pipeline->toDisplay8(comp);
                display = toQImage(out, false, nullptr);
                st.histogram = computeHistogram(out, 4);
            } else if (mode == LiveMode::Mosaic) {
                const bool force = m_mosaicForceAdd.exchange(false);
                auto status = m_mosaic.feed(lin, force);
                QMetaObject::invokeMethod(this, [this, status] { emit mosaicStatus(status); }, Qt::QueuedConnection);
                double scale = 1.0;
                Image16 prev = m_mosaic.preview(2400, scale);
                Image8 out = pipeline->toDisplay8(prev);
                display = toQImage(out, false, nullptr);
                st.histogram = computeHistogram(out, 4);
            } else {
                if (m_frozen)
                    continue;
                Image8 out = pipeline->toDisplay8(lin);
                unsharpMask(out, cs.sharpenAmount, cs.sharpenRadius);
                st.histogram = computeHistogram(out, 3);
                display = toQImage(out, m_showClipping, &lin);
            }

            const auto now = Clock::now();
            if (m_lastDisplayTime.time_since_epoch().count() != 0) {
                const double dt = std::chrono::duration<double>(now - m_lastDisplayTime).count();
                if (dt > 0)
                    m_displayFps = m_displayFps == 0 ? 1.0 / dt : 0.9 * m_displayFps + 0.1 / dt;
            }
            m_lastDisplayTime = now;
            st.displayFps = m_displayFps;
            emit frameReady(display, st);
        } catch (const std::exception &e) {
            emit cameraError(QString::fromUtf8(e.what()));
        }
    }
}

void AcquisitionEngine::finishMultifocus()
{
    auto res = std::make_shared<CaptureResult>();
    res->linear = m_stacker.result();
    if (res->linear.empty()) {
        emit captureFailed(tr("Multifocus stack is empty"));
        return;
    }
    std::shared_ptr<const ColorPipeline> pipeline;
    {
        QMutexLocker l(&m_mutex);
        pipeline = m_pipeline;
    }
    res->rendered16 = pipeline->toDisplay16(res->linear);
    res->rendered8 = pipeline->toDisplay8(res->linear);
    res->averagedFrames = m_stacker.frameCount();
    res->kind = "multifocus";
    if (m_camera) {
        res->exposureMs = m_camera->exposure();
        res->gain = m_camera->gain();
    }
    m_mode = LiveMode::Normal;
    emit captureFinished(res);
}

void AcquisitionEngine::finishMosaic()
{
    auto res = std::make_shared<CaptureResult>();
    res->linear = m_mosaic.result();
    if (res->linear.empty()) {
        emit captureFailed(tr("Mosaic is empty"));
        return;
    }
    std::shared_ptr<const ColorPipeline> pipeline;
    {
        QMutexLocker l(&m_mutex);
        pipeline = m_pipeline;
    }
    res->rendered16 = pipeline->toDisplay16(res->linear);
    res->rendered8 = pipeline->toDisplay8(res->linear);
    res->averagedFrames = m_mosaic.status().tiles;
    res->kind = "mosaic";
    if (m_camera) {
        res->exposureMs = m_camera->exposure();
        res->gain = m_camera->gain();
    }
    m_mode = LiveMode::Normal;
    emit captureFinished(res);
}

} // namespace lm
