#include "ImageJTiff.h"

#include <QByteArray>
#include <QFileInfo>
#include <QObject>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lm {

namespace {

enum : uint16_t {
    kSubfile = 254, kWidth = 256, kLength = 257, kBits = 258, kCompression = 259, kPhotometric = 262,
    kDescription = 270, kStripOffsets = 273, kSamples = 277, kRowsPerStrip = 278, kStripBytes = 279, kXRes = 282,
    kYRes = 283, kResUnit = 296, kSoftware = 305, kSampleFormat = 339, kIjByteCounts = 50838, kIjMetadata = 50839
};
enum : uint16_t { tByte = 1, tAscii = 2, tShort = 3, tLong = 4, tRational = 5 };

constexpr quint64 kMaxOffset = 0xFFFFFFFFull;

void put16(QByteArray &b, uint16_t v)
{
    b.append(char(v & 0xFF));
    b.append(char(v >> 8));
}
void put32(QByteArray &b, uint32_t v)
{
    for (int k = 0; k < 4; ++k)
        b.append(char((v >> (8 * k)) & 0xFF));
}
void putDouble(QByteArray &b, double d)
{
    quint64 u;
    std::memcpy(&u, &d, 8);
    for (int k = 0; k < 8; ++k)
        b.append(char((u >> (8 * k)) & 0xFF));
}

struct Tag {
    uint16_t tag, type;
    uint32_t count;
    QByteArray data; // little-endian payload
};
int typeSize(uint16_t t) { return t == tShort ? 2 : t == tLong ? 4 : t == tRational ? 8 : 1; }

QByteArray shorts(std::initializer_list<uint16_t> v)
{
    QByteArray b;
    for (uint16_t x : v)
        put16(b, x);
    return b;
}
QByteArray longs(const std::vector<uint32_t> &v)
{
    QByteArray b;
    for (uint32_t x : v)
        put32(b, x);
    return b;
}
// A rational close to v, as ImageJ writes the pixels per unit.
QByteArray rational(double v)
{
    uint32_t den = 1000000;
    while (den > 1 && v * den > 4.0e9)
        den /= 10;
    return longs({uint32_t(std::lround(std::max(0.0, v) * den)), den});
}

// ImageJ's description: what makes it a hyperstack and how it is calibrated.
QString description(int images, int channels, int slices, int frames, int bits, const StackCalibration &cal,
                    const QList<StackChannel> &luts)
{
    QString d = QStringLiteral("ImageJ=1.54f\nimages=%1\n").arg(images);
    if (channels > 1)
        d += QStringLiteral("channels=%1\n").arg(channels);
    if (slices > 1)
        d += QStringLiteral("slices=%1\n").arg(slices);
    if (frames > 1)
        d += QStringLiteral("frames=%1\n").arg(frames);
    if (channels > 1 || slices > 1 || frames > 1)
        d += QStringLiteral("hyperstack=true\n");
    if (channels > 1)
        d += QStringLiteral("mode=composite\n");
    if (cal.umPerPixel > 0)
        d += QStringLiteral("unit=micron\n");
    if (slices > 1 && cal.zStepUm > 0)
        d += QStringLiteral("spacing=%1\n").arg(cal.zStepUm, 0, 'g', 10);
    if (frames > 1 && cal.frameIntervalS > 0)
        d += QStringLiteral("finterval=%1\n").arg(cal.frameIntervalS, 0, 'g', 10);
    d += QStringLiteral("loop=false\n");
    if (channels == 1 && !luts.isEmpty()) {
        d += QStringLiteral("min=%1\nmax=%2\n").arg(luts[0].displayMin, 0, 'g', 10).arg(luts[0].displayMax, 0, 'g', 10);
    } else if (channels == 1) {
        d += QStringLiteral("min=0\nmax=%1\n").arg(bits > 8 ? 65535 : 255);
    }
    return d;
}

// ImageJ's private metadata: display ranges and one LUT per channel. The
// numbers are in the file's byte order, little endian here.
void imageJMetadata(const QList<StackChannel> &luts, QByteArray &data, std::vector<uint32_t> &counts)
{
    constexpr uint32_t kMagic = 0x494a494a; // "IJIJ"
    constexpr uint32_t kRanges = 0x72616e67; // "rang"
    constexpr uint32_t kLuts = 0x6c757473;   // "luts"
    const int n = int(luts.size());
    QByteArray header;
    put32(header, kMagic);
    put32(header, kRanges);
    put32(header, 1);
    put32(header, kLuts);
    put32(header, uint32_t(n));
    data = header;
    counts.push_back(uint32_t(header.size()));
    QByteArray ranges;
    for (const StackChannel &c : luts) {
        putDouble(ranges, c.displayMin);
        putDouble(ranges, c.displayMax);
    }
    data += ranges;
    counts.push_back(uint32_t(ranges.size()));
    for (const StackChannel &c : luts) {
        QByteArray lut(768, 0);
        for (int i = 0; i < 256; ++i) {
            lut[i] = char(qRed(c.colour) * i / 255);
            lut[256 + i] = char(qGreen(c.colour) * i / 255);
            lut[512 + i] = char(qBlue(c.colour) * i / 255);
        }
        data += lut;
        counts.push_back(768);
    }
}

} // namespace

bool writeImageJStack(const QString &path, int width, int height, int bits, int channels, int slices, int frames,
                      const StackPlaneSource &source, const StackCalibration &cal, const QList<StackChannel> &luts,
                      bool deflate, QString *error)
{
    const auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (width <= 0 || height <= 0 || channels <= 0 || slices <= 0 || frames <= 0)
        return fail(QObject::tr("Empty image"));
    bits = bits > 8 ? 16 : 8;
    const qint64 pages = qint64(channels) * slices * frames;
    if (pages > 1000000)
        return fail(QObject::tr("Too many planes for one file (%1).").arg(pages));
    // 4 GB of 32-bit offsets: uncompressed this is known now; compressed it is
    // checked as the file grows
    if (!deflate && imageJStackBytes(width, height, bits, pages) + pages * 512 + 65536 > qint64(kMaxOffset))
        return fail(QObject::tr("The stack is larger than a TIFF file can hold (4 GB); export it in parts."));

    const int bps = bits / 8;
    const qint64 rowBytes = qint64(width) * bps;
    const int rowsPerStrip = int(std::clamp<qint64>((256 * 1024) / std::max<qint64>(1, rowBytes), 1, height));
    const int strips = (height + rowsPerStrip - 1) / rowsPerStrip;

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return fail(QObject::tr("Cannot write %1: %2").arg(QFileInfo(path).fileName(), f.errorString()));
    QByteArray head("II", 2);
    put16(head, 42);
    put32(head, 8); // the first page follows
    f.write(head);
    quint64 pos = 8;

    const QString desc = description(int(pages), channels, slices, frames, bits, cal, luts);
    QByteArray descBytes = desc.toUtf8();
    descBytes.append('\0');
    QByteArray software = QByteArrayLiteral("DM Imaging");
    software.append('\0');
    QByteArray ijData;
    std::vector<uint32_t> ijCounts;
    if (!luts.isEmpty() && luts.size() == channels)
        imageJMetadata(luts, ijData, ijCounts);
    const double ppuX = cal.umPerPixel > 0 ? 1.0 / cal.umPerPixel : 1.0;
    const double ppuY = (cal.umPerPixelY > 0 ? cal.umPerPixelY : cal.umPerPixel) > 0
                            ? 1.0 / (cal.umPerPixelY > 0 ? cal.umPerPixelY : cal.umPerPixel)
                            : 1.0;

    std::vector<uint16_t> plane;
    for (qint64 page = 0; page < pages; ++page) {
        plane.assign(size_t(width) * height, 0);
        QString why;
        if (!source(int(page), plane, &why)) {
            f.cancelWriting();
            if (error)
                *error = why;
            return false;
        }
        // the strips of this page, compressed one by one
        std::vector<QByteArray> stripData(static_cast<size_t>(strips));
        for (int s = 0; s < strips; ++s) {
            const int y0 = s * rowsPerStrip, y1 = std::min(height, y0 + rowsPerStrip);
            QByteArray raw(int(rowBytes * (y1 - y0)), Qt::Uninitialized);
            uchar *d = reinterpret_cast<uchar *>(raw.data());
            const uint16_t *src = plane.data() + size_t(y0) * width;
            const size_t n = size_t(width) * (y1 - y0);
            if (bits == 8) {
                for (size_t i = 0; i < n; ++i)
                    d[i] = uchar(std::min<uint16_t>(src[i], 255));
            } else {
                for (size_t i = 0; i < n; ++i) {
                    d[2 * i] = uchar(src[i] & 0xFF);
                    d[2 * i + 1] = uchar(src[i] >> 8);
                }
            }
            // a plain zlib stream: qCompress's 4-byte length prefix removed
            stripData[size_t(s)] = deflate ? qCompress(raw, 6).mid(4) : raw;
        }

        // page layout: IFD | out-of-line tag data | strips
        std::vector<Tag> tags;
        tags.push_back({kSubfile, tLong, 1, longs({0})});
        tags.push_back({kWidth, tLong, 1, longs({uint32_t(width)})});
        tags.push_back({kLength, tLong, 1, longs({uint32_t(height)})});
        tags.push_back({kBits, tShort, 1, shorts({uint16_t(bits)})});
        tags.push_back({kCompression, tShort, 1, shorts({uint16_t(deflate ? 8 : 1)})});
        tags.push_back({kPhotometric, tShort, 1, shorts({1})}); // black is zero
        if (page == 0)
            tags.push_back({kDescription, tAscii, uint32_t(descBytes.size()), descBytes});
        tags.push_back({kStripOffsets, tLong, uint32_t(strips), QByteArray()}); // filled in below
        tags.push_back({kSamples, tShort, 1, shorts({1})});
        tags.push_back({kRowsPerStrip, tLong, 1, longs({uint32_t(rowsPerStrip)})});
        std::vector<uint32_t> counts;
        for (const QByteArray &sd : stripData)
            counts.push_back(uint32_t(sd.size()));
        tags.push_back({kStripBytes, tLong, uint32_t(strips), longs(counts)});
        tags.push_back({kXRes, tRational, 1, rational(ppuX)});
        tags.push_back({kYRes, tRational, 1, rational(ppuY)});
        tags.push_back({kResUnit, tShort, 1, shorts({1})}); // none: the unit is in the description
        if (page == 0)
            tags.push_back({kSoftware, tAscii, uint32_t(software.size()), software});
        tags.push_back({kSampleFormat, tShort, 1, shorts({1})});
        if (page == 0 && !ijData.isEmpty()) {
            tags.push_back({kIjByteCounts, tLong, uint32_t(ijCounts.size()), longs(ijCounts)});
            tags.push_back({kIjMetadata, tByte, uint32_t(ijData.size()), ijData});
        }

        const quint64 ifdOffset = pos;
        const quint64 ifdSize = 2 + quint64(tags.size()) * 12 + 4;
        // out-of-line data size, with the strip offsets as a placeholder
        quint64 extraSize = 0;
        for (const Tag &t : tags) {
            const quint64 size = quint64(t.count) * typeSize(t.type);
            if (size > 4)
                extraSize += size + (size & 1);
        }
        quint64 dataStart = ifdOffset + ifdSize + extraSize;
        std::vector<uint32_t> offsets;
        quint64 at = dataStart;
        for (const QByteArray &sd : stripData) {
            offsets.push_back(uint32_t(at));
            at += quint64(sd.size());
            if (at & 1)
                ++at;
        }
        if (at > kMaxOffset) {
            f.cancelWriting();
            return fail(QObject::tr("The stack is larger than a TIFF file can hold (4 GB); export it in parts."));
        }
        for (Tag &t : tags)
            if (t.tag == kStripOffsets)
                t.data = longs(offsets);
        const quint64 next = page + 1 < pages ? at : 0;

        QByteArray ifd, extra;
        put16(ifd, uint16_t(tags.size()));
        for (const Tag &t : tags) {
            put16(ifd, t.tag);
            put16(ifd, t.type);
            put32(ifd, t.count);
            const int size = int(t.count) * typeSize(t.type);
            if (size <= 4) {
                QByteArray v = t.data.left(4);
                v.resize(4, '\0');
                ifd.append(v);
            } else {
                put32(ifd, uint32_t(ifdOffset + ifdSize + quint64(extra.size())));
                extra.append(t.data);
                if (extra.size() & 1)
                    extra.append('\0');
            }
        }
        put32(ifd, uint32_t(next));
        f.write(ifd);
        f.write(extra);
        for (const QByteArray &sd : stripData) {
            f.write(sd);
            if (sd.size() & 1)
                f.write("\0", 1);
        }
        pos = at;
        if (f.error() != QFileDevice::NoError)
            break; // reported by commit()
    }
    if (!f.commit())
        return fail(QObject::tr("Writing %1 failed: %2").arg(QFileInfo(path).fileName(), f.errorString()));
    return true;
}

} // namespace lm
