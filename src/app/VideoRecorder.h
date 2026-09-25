#pragma once
// Records the live image to a Motion-JPEG AVI on a background thread.
// Frames are rate limited to the requested frame rate; if encoding falls
// behind, frames are dropped instead of blocking the live view.

#include "app/AppSettings.h"

#include <QImage>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace lm {

class VideoRecorder {
public:
    ~VideoRecorder();
    bool start(const QString &path, double fps, bool scaleBar, QString *error = nullptr);
    // call for every displayed frame; umPerPixel of that frame (for the scale bar)
    void push(const QImage &frame, double umPerPixel);
    void stop();                    // finishes the file
    bool isRecording() const { return m_running; }
    int frames() const { return m_frames; }
    int dropped() const { return m_dropped; }
    double seconds() const;
    QString path() const { return m_path; }
    qint64 bytes() const { return m_bytes; }

private:
    void run();

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<QImage, double>> m_queue;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_frames{0}, m_dropped{0};
    std::atomic<qint64> m_bytes{0};
    double m_fps = 25;
    bool m_scaleBar = false;
    OverlaySettings m_overlay;
    QString m_path, m_error;
    qint64 m_startMs = 0, m_lastPushMs = 0;
};

} // namespace lm
