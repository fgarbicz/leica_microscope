#pragma once
// Minimal Motion-JPEG AVI writer (RIFF AVI 1.0 with idx1 index), playable in
// Windows Media Player, VLC, PowerPoint and ImageJ. Frames are JPEG encoded by
// the caller or via addFrame(QImage).

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QString>

#include <vector>

namespace lm {

class AviWriter {
public:
    ~AviWriter();
    bool open(const QString &path, int width, int height, double fps, QString *error = nullptr);
    bool addJpeg(const QByteArray &jpeg);
    bool addFrame(const QImage &img, int quality = 90);
    // Writes the index and final header values. actualFps > 0 replaces the
    // nominal frame rate (so playback runs in real time when frames were dropped).
    bool close(double actualFps = 0.0);
    bool isOpen() const { return m_file.isOpen(); }
    int frameCount() const { return int(m_index.size()); }
    qint64 bytesWritten() const { return m_file.isOpen() ? m_file.pos() : 0; }
    static QByteArray encodeJpeg(const QImage &img, int quality);

private:
    void writeHeaders();
    QFile m_file;
    int m_w = 0, m_h = 0;
    double m_fps = 25;
    qint64 m_moviPos = 0;   // file offset of the 'movi' list type
    struct Entry { quint32 offset, size; };
    std::vector<Entry> m_index;
    quint32 m_maxFrame = 0;
};

} // namespace lm
