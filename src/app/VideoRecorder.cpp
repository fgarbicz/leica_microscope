#include "VideoRecorder.h"

#include "io/AviWriter.h"
#include "ui/Overlays.h"

#include <QDateTime>

namespace lm {

VideoRecorder::~VideoRecorder()
{
    stop();
}

bool VideoRecorder::start(const QString &path, double fps, bool scaleBar, QString *error)
{
    stop();
    m_path = path;
    m_fps = std::clamp(fps, 1.0, 60.0);
    m_scaleBar = scaleBar;
    m_overlay = AppSettings::instance().overlays;
    m_frames = 0;
    m_dropped = 0;
    m_bytes = 0;
    m_startMs = QDateTime::currentMSecsSinceEpoch();
    m_lastPushMs = 0;
    // the file is opened lazily on the first frame (size known then); check the path now
    QFile probe(path);
    if (!probe.open(QIODevice::WriteOnly)) {
        if (error)
            *error = probe.errorString();
        return false;
    }
    probe.close();
    m_running = true;
    m_thread = std::thread([this] { run(); });
    return true;
}

void VideoRecorder::push(const QImage &frame, double umPerPixel)
{
    if (!m_running || frame.isNull())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double interval = 1000.0 / m_fps;
    if (m_lastPushMs && now - m_lastPushMs < interval * 0.95)
        return; // rate limit to the recording frame rate
    m_lastPushMs = m_lastPushMs ? m_lastPushMs + qint64(interval) : now;
    if (now - m_lastPushMs > interval * 3)
        m_lastPushMs = now; // resynchronise after a stall
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_queue.size() >= 6) {
        ++m_dropped;
        return;
    }
    m_queue.emplace_back(frame, umPerPixel);
    m_cv.notify_one();
}

double VideoRecorder::seconds() const
{
    return m_running ? (QDateTime::currentMSecsSinceEpoch() - m_startMs) / 1000.0 : m_frames / m_fps;
}

void VideoRecorder::stop()
{
    if (!m_thread.joinable())
        return;
    m_running = false;
    m_cv.notify_all();
    m_thread.join();
}

void VideoRecorder::run()
{
    AviWriter avi;
    for (;;) {
        QImage img;
        double um = 0;
        {
            std::unique_lock<std::mutex> l(m_mutex);
            m_cv.wait(l, [this] { return !m_queue.empty() || !m_running; });
            if (m_queue.empty() && !m_running)
                break;
            img = std::move(m_queue.front().first);
            um = m_queue.front().second;
            m_queue.pop_front();
        }
        if (!avi.isOpen()) {
            // even dimensions for maximum player compatibility
            if (!avi.open(m_path, img.width() & ~1, img.height() & ~1, m_fps, &m_error))
                break;
        }
        if (m_scaleBar && um > 0)
            img = burnScaleBar(img, um, m_overlay);
        avi.addFrame(img, 88);
        ++m_frames;
        m_bytes = avi.bytesWritten();
    }
    if (avi.isOpen()) {
        // real-time playback: use the frame rate actually achieved
        const double secs = (QDateTime::currentMSecsSinceEpoch() - m_startMs) / 1000.0;
        const double actual = secs > 0.5 ? m_frames / secs : 0.0;
        avi.close(actual > 0 && actual < m_fps * 0.97 ? actual : 0.0);
    }
    else
        QFile::remove(m_path); // nothing recorded: do not leave an empty file
}

} // namespace lm
