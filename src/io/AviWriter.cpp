#include "AviWriter.h"

#include <QBuffer>
#include <QImageWriter>

#include <cmath>

namespace lm {

namespace {
void u32(QFile &f, quint32 v)
{
    const char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char((v >> 24) & 0xFF)};
    f.write(b, 4);
}
void u16(QFile &f, quint16 v)
{
    const char b[2] = {char(v & 0xFF), char((v >> 8) & 0xFF)};
    f.write(b, 2);
}
void fcc(QFile &f, const char *s) { f.write(s, 4); }
void patch32(QFile &f, qint64 pos, quint32 v)
{
    const qint64 cur = f.pos();
    f.seek(pos);
    u32(f, v);
    f.seek(cur);
}

// header layout offsets (fixed because every header has a fixed size)
constexpr qint64 kRiffSize = 4;
constexpr qint64 kAvihTotalFrames = 48;    // 'avih' data starts at 32
constexpr qint64 kAvihSuggestedBuf = 60;
constexpr qint64 kStrhLength = 140;        // 'strh' data starts at 108
constexpr qint64 kStrhSuggestedBuf = 144;
constexpr qint64 kMoviListSize = 216;      // after hdrl (212 bytes from 12)
} // namespace

AviWriter::~AviWriter()
{
    if (m_file.isOpen())
        close();
}

QByteArray AviWriter::encodeJpeg(const QImage &img, int quality)
{
    QByteArray data;
    QBuffer buf(&data);
    buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, "jpeg");
    w.setQuality(quality);
    w.write(img.convertToFormat(QImage::Format_RGB888));
    return data;
}

bool AviWriter::open(const QString &path, int width, int height, double fps, QString *error)
{
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = m_file.errorString();
        return false;
    }
    m_w = width;
    m_h = height;
    m_fps = std::clamp(fps, 1.0, 120.0);
    m_index.clear();
    m_maxFrame = 0;
    writeHeaders();
    return true;
}

void AviWriter::writeHeaders()
{
    QFile &f = m_file;
    fcc(f, "RIFF");
    u32(f, 0); // patched
    fcc(f, "AVI ");
    // hdrl list: 4 ('hdrl') + 8+56 (avih) + 8+116 (strl list) = 192
    fcc(f, "LIST");
    u32(f, 4 + 64 + 124);
    fcc(f, "hdrl");
    fcc(f, "avih");
    u32(f, 56);
    u32(f, quint32(std::lround(1e6 / m_fps))); // microseconds per frame
    u32(f, 0);                                 // max bytes per second
    u32(f, 0);                                 // padding granularity
    u32(f, 0x10);                              // AVIF_HASINDEX
    u32(f, 0);                                 // total frames (patched)
    u32(f, 0);                                 // initial frames
    u32(f, 1);                                 // streams
    u32(f, 0);                                 // suggested buffer size (patched)
    u32(f, quint32(m_w));
    u32(f, quint32(m_h));
    for (int i = 0; i < 4; ++i)
        u32(f, 0);
    // strl list: 4 ('strl') + 8+56 (strh) + 8+40 (strf) = 116
    fcc(f, "LIST");
    u32(f, 116);
    fcc(f, "strl");
    fcc(f, "strh");
    u32(f, 56);
    fcc(f, "vids");
    fcc(f, "MJPG");
    u32(f, 0);          // flags
    u16(f, 0);          // priority
    u16(f, 0);          // language
    u32(f, 0);          // initial frames
    u32(f, 1000);       // scale
    u32(f, quint32(std::lround(m_fps * 1000))); // rate
    u32(f, 0);          // start
    u32(f, 0);          // length (patched)
    u32(f, 0);          // suggested buffer size (patched)
    u32(f, 0xFFFFFFFFu); // quality
    u32(f, 0);          // sample size
    u16(f, 0);
    u16(f, 0);
    u16(f, quint16(m_w));
    u16(f, quint16(m_h));
    fcc(f, "strf");
    u32(f, 40);
    u32(f, 40);
    u32(f, quint32(m_w));
    u32(f, quint32(m_h));
    u16(f, 1);
    u16(f, 24);
    fcc(f, "MJPG");
    u32(f, quint32(m_w * m_h * 3));
    u32(f, 0);
    u32(f, 0);
    u32(f, 0);
    u32(f, 0);
    // movi list
    fcc(f, "LIST");
    u32(f, 0); // patched
    m_moviPos = f.pos();
    fcc(f, "movi");
}

bool AviWriter::addJpeg(const QByteArray &jpeg)
{
    if (!m_file.isOpen() || jpeg.isEmpty())
        return false;
    // AVI 1.0 offsets and sizes are 32-bit (signed in many readers): refuse to grow past 2 GB
    if (m_file.pos() + jpeg.size() + 8 + qint64(m_index.size() + 1) * 16 + 16 > 0x7FFFFFFFLL)
        return false;
    const quint32 offset = quint32(m_file.pos() - m_moviPos);
    fcc(m_file, "00dc");
    u32(m_file, quint32(jpeg.size()));
    m_file.write(jpeg);
    if (jpeg.size() & 1)
        m_file.write("\0", 1);
    m_index.push_back({offset, quint32(jpeg.size())});
    m_maxFrame = std::max(m_maxFrame, quint32(jpeg.size()));
    return m_file.error() == QFileDevice::NoError;
}

bool AviWriter::addFrame(const QImage &img, int quality)
{
    QImage frame = img;
    if (img.width() != m_w || img.height() != m_h)
        frame = img.scaled(m_w, m_h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return addJpeg(encodeJpeg(frame, quality));
}

bool AviWriter::close(double actualFps)
{
    if (!m_file.isOpen())
        return false;
    const qint64 moviEnd = m_file.pos();
    fcc(m_file, "idx1");
    u32(m_file, quint32(m_index.size() * 16));
    for (const auto &e : m_index) {
        fcc(m_file, "00dc");
        u32(m_file, 0x10); // keyframe
        u32(m_file, e.offset);
        u32(m_file, e.size);
    }
    const qint64 end = m_file.pos();
    patch32(m_file, kRiffSize, quint32(end - 8));
    patch32(m_file, kAvihTotalFrames, quint32(m_index.size()));
    patch32(m_file, kAvihSuggestedBuf, m_maxFrame + 8);
    patch32(m_file, kStrhLength, quint32(m_index.size()));
    patch32(m_file, kStrhSuggestedBuf, m_maxFrame + 8);
    patch32(m_file, kMoviListSize, quint32(moviEnd - m_moviPos));
    if (actualFps > 0.1) {
        patch32(m_file, 32, quint32(std::lround(1e6 / actualFps)));       // avih: us per frame
        patch32(m_file, 132, quint32(std::lround(actualFps * 1000)));     // strh: rate (scale 1000)
    }
    const bool ok = m_file.error() == QFileDevice::NoError;
    m_file.close();
    return ok;
}

} // namespace lm
