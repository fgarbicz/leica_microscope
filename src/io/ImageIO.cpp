#include "ImageIO.h"

#include "io/LifFile.h"

#include <QColorSpace>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace lm {

namespace {

// ---------------------------------------------------------------- TIFF tags
enum : uint16_t {
    kWidth = 256, kLength = 257, kBits = 258, kCompression = 259, kPhotometric = 262, kDescription = 270,
    kStripOffsets = 273, kSamples = 277, kRowsPerStrip = 278, kStripBytes = 279, kXRes = 282, kYRes = 283,
    kPlanar = 284, kResUnit = 296, kSoftware = 305, kDateTime = 306, kPredictor = 317, kSampleFormat = 339
};
enum : uint16_t { tByte = 1, tAscii = 2, tShort = 3, tLong = 4, tRational = 5 };

struct Entry {
    uint16_t tag, type;
    uint32_t count;
    QByteArray data; // raw little-endian payload
};

QByteArray le16(const std::vector<uint16_t> &v)
{
    QByteArray b(int(v.size() * 2), 0);
    for (size_t i = 0; i < v.size(); ++i) {
        b[int(2 * i)] = char(v[i] & 0xFF);
        b[int(2 * i + 1)] = char(v[i] >> 8);
    }
    return b;
}
QByteArray le32(const std::vector<uint32_t> &v)
{
    QByteArray b(int(v.size() * 4), 0);
    for (size_t i = 0; i < v.size(); ++i)
        for (int k = 0; k < 4; ++k)
            b[int(4 * i + k)] = char((v[i] >> (8 * k)) & 0xFF);
    return b;
}
int typeSize(uint16_t t) { return t == tShort ? 2 : t == tLong ? 4 : t == tRational ? 8 : 1; }

QByteArray deflate(const QByteArray &raw)
{
    QByteArray c = qCompress(raw, 6);
    return c.mid(4); // strip Qt's 4 byte length prefix -> plain zlib stream
}

QByteArray inflate(const QByteArray &z, int expected)
{
    QByteArray withLen(4, 0);
    withLen[0] = char((expected >> 24) & 0xFF);
    withLen[1] = char((expected >> 16) & 0xFF);
    withLen[2] = char((expected >> 8) & 0xFF);
    withLen[3] = char(expected & 0xFF);
    withLen += z;
    return qUncompress(withLen);
}

QString tooLargeForTiff()
{
    return QObject::tr("Image too large for TIFF (over 4 GB); save as smaller tiles or use compression");
}

} // namespace

// ============================================================ TIFF writer
qint64 tiffUncompressedSize(qint64 width, qint64 height, int bits)
{
    const qint64 bps = bits > 8 ? 2 : 1;
    // header + pixel data + generous allowance for the IFD, strip tables and metadata
    return 8 + width * height * 3 * bps + 1024 * 1024;
}

bool writeTiff(const QString &path, const Image16 &img, int bits, bool useDeflate, double umPerPixel,
               const QString &description, QString *error)
{
    if (img.empty()) {
        if (error) *error = QObject::tr("Empty image");
        return false;
    }
    bits = bits > 8 ? 16 : 8;
    // classic TIFF uses 32-bit offsets: refuse up front when the raw data cannot fit
    if (!useDeflate && tiffUncompressedSize(img.width, img.height, bits) > kTiffMaxBytes) {
        if (error) *error = tooLargeForTiff();
        return false;
    }
    const int w = img.width, h = img.height, bps = bits / 8;
    const int rowBytes = w * 3 * bps;
    const int rowsPerStrip = std::max(1, std::min(h, (256 * 1024) / std::max(1, rowBytes)));
    const int strips = (h + rowsPerStrip - 1) / rowsPerStrip;

    std::vector<QByteArray> stripData(static_cast<size_t>(strips));
    // encode strips in parallel-friendly independent chunks
    for (int s = 0; s < strips; ++s) {
        const int y0 = s * rowsPerStrip, y1 = std::min(h, y0 + rowsPerStrip);
        QByteArray raw(rowBytes * (y1 - y0), 0);
        uchar *d = reinterpret_cast<uchar *>(raw.data());
        for (int y = y0; y < y1; ++y) {
            const uint16_t *src = img.row(y);
            uchar *row = d + size_t(y - y0) * rowBytes;
            if (bits == 16) {
                // horizontal predictor on 16-bit samples when compressing
                uint16_t prev[3] = {0, 0, 0};
                for (int i = 0; i < w * 3; ++i) {
                    uint16_t v = src[i];
                    if (useDeflate) {
                        const uint16_t diff = uint16_t(v - prev[i % 3]);
                        prev[i % 3] = v;
                        v = diff;
                    }
                    row[2 * i] = uchar(v & 0xFF);
                    row[2 * i + 1] = uchar(v >> 8);
                }
            } else {
                uint8_t prev[3] = {0, 0, 0};
                for (int i = 0; i < w * 3; ++i) {
                    uint8_t v = uint8_t((src[i] + 128) / 257);
                    if (useDeflate) {
                        const uint8_t diff = uint8_t(v - prev[i % 3]);
                        prev[i % 3] = v;
                        v = diff;
                    }
                    row[i] = v;
                }
            }
        }
        stripData[size_t(s)] = useDeflate ? deflate(raw) : raw;
    }

    // layout: header(8) | strips | IFD | out-of-line tag data
    // offsets are computed in 64 bit and checked: they must fit the 32-bit TIFF fields
    uint64_t offset = 8;
    std::vector<uint32_t> stripOffsets, stripCounts;
    for (auto &sd : stripData) {
        stripOffsets.push_back(uint32_t(offset));
        stripCounts.push_back(uint32_t(sd.size()));
        offset += uint64_t(sd.size());
        if (offset & 1)
            ++offset; // word alignment
    }
    // the IFD and its out-of-line data (description, strip tables) follow the strips
    const uint64_t tail = 1024 + uint64_t(description.size()) * 4 + uint64_t(strips) * 8;
    if (offset + tail > uint64_t(kTiffMaxBytes)) {
        if (error) *error = tooLargeForTiff();
        return false;
    }
    const uint32_t ifdOffset = uint32_t(offset);

    QByteArray desc = description.toUtf8();
    desc.append('\0');
    QByteArray software = QByteArrayLiteral("DM Imaging");
    software.append('\0');
    QByteArray dt = QDateTime::currentDateTime().toString(QStringLiteral("yyyy:MM:dd HH:mm:ss")).toLatin1();
    dt.append('\0');
    // calibrated: pixels per centimetre. Uncalibrated: unit "none" with a neutral
    // 1:1 ratio, so no reader mistakes it for a physical pixel size.
    const bool calibrated = umPerPixel > 0;
    const double ppcm = calibrated ? 10000.0 / umPerPixel : 1.0;
    const uint32_t resNum = uint32_t(std::min(4.0e9, std::round(ppcm * 1000.0))), resDen = 1000;

    std::vector<Entry> e;
    e.push_back({kWidth, tLong, 1, le32({uint32_t(w)})});
    e.push_back({kLength, tLong, 1, le32({uint32_t(h)})});
    e.push_back({kBits, tShort, 3, le16({uint16_t(bits), uint16_t(bits), uint16_t(bits)})});
    e.push_back({kCompression, tShort, 1, le16({uint16_t(useDeflate ? 8 : 1)})});
    e.push_back({kPhotometric, tShort, 1, le16({2})});
    if (!description.isEmpty())
        e.push_back({kDescription, tAscii, uint32_t(desc.size()), desc});
    e.push_back({kStripOffsets, tLong, uint32_t(strips), le32(stripOffsets)});
    e.push_back({kSamples, tShort, 1, le16({3})});
    e.push_back({kRowsPerStrip, tLong, 1, le32({uint32_t(rowsPerStrip)})});
    e.push_back({kStripBytes, tLong, uint32_t(strips), le32(stripCounts)});
    e.push_back({kXRes, tRational, 1, le32({resNum, resDen})});
    e.push_back({kYRes, tRational, 1, le32({resNum, resDen})});
    e.push_back({kPlanar, tShort, 1, le16({1})});
    e.push_back({kResUnit, tShort, 1, le16({uint16_t(calibrated ? 3 : 1)})}); // centimetre / none
    e.push_back({kSoftware, tAscii, uint32_t(software.size()), software});
    e.push_back({kDateTime, tAscii, uint32_t(dt.size()), dt});
    if (useDeflate)
        e.push_back({kPredictor, tShort, 1, le16({2})});
    e.push_back({kSampleFormat, tShort, 3, le16({1, 1, 1})});

    const uint32_t ifdSize = 2 + uint32_t(e.size()) * 12 + 4;
    uint32_t extra = ifdOffset + ifdSize;
    QByteArray ifd, ext;
    auto put16 = [](QByteArray &b, uint16_t v) { b.append(char(v & 0xFF)); b.append(char(v >> 8)); };
    auto put32 = [](QByteArray &b, uint32_t v) { for (int k = 0; k < 4; ++k) b.append(char((v >> (8 * k)) & 0xFF)); };
    put16(ifd, uint16_t(e.size()));
    for (auto &en : e) {
        put16(ifd, en.tag);
        put16(ifd, en.type);
        put32(ifd, en.count);
        const int size = int(en.count) * typeSize(en.type);
        if (size <= 4) {
            QByteArray v = en.data.left(4);
            v.resize(4, '\0');
            ifd.append(v);
        } else {
            put32(ifd, extra + uint32_t(ext.size()));
            ext.append(en.data);
            if (ext.size() & 1)
                ext.append('\0');
        }
    }
    put32(ifd, 0); // no next IFD

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QByteArray hdr("II", 2);
    put16(hdr, 42);
    put32(hdr, ifdOffset);
    f.write(hdr);
    for (auto &sd : stripData) {
        f.write(sd);
        if (sd.size() & 1)
            f.write("\0", 1);
    }
    f.write(ifd);
    f.write(ext);
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

// ============================================================ TIFF reader
bool readTiff(const QString &path, Image16 &img, int &bits, QString &description, double &umPerPixel, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    const QByteArray file = f.readAll();
    auto fail = [&](const QString &m) { if (error) *error = m; return false; };
    if (file.size() < 8)
        return fail(QObject::tr("Not a TIFF file"));
    const bool le = file[0] == 'I';
    const uchar *d = reinterpret_cast<const uchar *>(file.constData());
    const qsizetype n = file.size();
    auto g16 = [&](qsizetype o) -> uint32_t {
        if (o + 2 > n) return 0;
        return le ? uint32_t(d[o] | d[o + 1] << 8) : uint32_t(d[o] << 8 | d[o + 1]);
    };
    auto g32 = [&](qsizetype o) -> uint32_t {
        if (o + 4 > n) return 0;
        return le ? uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24)
                  : uint32_t(uint32_t(d[o]) << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]);
    };
    if (g16(2) != 42)
        return fail(QObject::tr("Unsupported TIFF variant"));
    const uint32_t ifd = g32(4);
    const uint32_t count = g16(ifd);
    uint32_t w = 0, h = 0, comp = 1, photometric = 2, spp = 1, rps = 0, predictor = 1, planar = 1, resUnit = 2;
    std::vector<uint32_t> bitsV, offs, cnts;
    double xres = 0;
    auto values = [&](qsizetype entry) {
        const uint32_t type = g16(entry + 2), cnt = g32(entry + 4);
        const int ts = typeSize(uint16_t(type));
        const qsizetype base = qsizetype(cnt) * ts <= 4 ? entry + 8 : qsizetype(g32(entry + 8));
        std::vector<uint32_t> v;
        for (uint32_t i = 0; i < cnt && i < 1000000; ++i) {
            const qsizetype o = base + qsizetype(i) * ts;
            if (o < 0 || o + ts > n)
                break; // out of file: malformed tag
            v.push_back(type == tShort ? g16(o) : type == tLong ? g32(o) : d[o]);
        }
        return v;
    };
    auto first = [](const std::vector<uint32_t> &v) { return v.empty() ? 0u : v[0]; };
    if (qsizetype(ifd) + 2 + 12 * qsizetype(count) > n)
        return fail(QObject::tr("Corrupt TIFF directory"));
    for (uint32_t i = 0; i < count; ++i) {
        const qsizetype en = ifd + 2 + 12 * qsizetype(i);
        const uint32_t tag = g16(en);
        switch (tag) {
        case kWidth: w = first(values(en)); break;
        case kLength: h = first(values(en)); break;
        case kBits: bitsV = values(en); break;
        case kCompression: comp = first(values(en)); break;
        case kPhotometric: photometric = first(values(en)); break;
        case kStripOffsets: offs = values(en); break;
        case kSamples: spp = first(values(en)); break;
        case kRowsPerStrip: rps = first(values(en)); break;
        case kStripBytes: cnts = values(en); break;
        case kPredictor: predictor = first(values(en)); break;
        case kPlanar: planar = first(values(en)); break;
        case kResUnit: resUnit = first(values(en)); break;
        case kXRes: {
            const qsizetype o = g32(en + 8);
            const uint32_t num = g32(o), den = g32(o + 4); // g32 returns 0 when out of range
            xres = den ? double(num) / den : 0;
            break;
        }
        case kDescription: {
            const uint32_t cnt = g32(en + 4);
            const qsizetype o = cnt <= 4 ? en + 8 : qsizetype(g32(en + 8));
            if (o >= 0 && cnt < (64u << 20) && o + qsizetype(cnt) <= n)
                description = QString::fromUtf8(reinterpret_cast<const char *>(d + o), int(cnt)).trimmed().remove(QChar(0));
            break;
        }
        default: break;
        }
    }
    const uint32_t b = bitsV.empty() ? 1 : bitsV[0];
    if (w == 0 || h == 0 || (b != 8 && b != 16) || planar != 1 || (comp != 1 && comp != 8 && comp != 32946)
        || offs.empty() || offs.size() != cnts.size() || (spp != 1 && spp != 3 && spp != 4) || photometric > 2)
        return fail(QObject::tr("Unsupported TIFF layout"));
    // Size limits. The writer accepts any Image16, and a Deflate-compressed
    // mosaic can hold far more than 4 GB of pixels, so the reader must not refuse
    // what the writer produced. What it can refuse is a size the image types
    // cannot hold, and a size the file cannot possibly contain: a tiny corrupt
    // file must not request gigabytes.
    if (rps == 0 || rps > h)
        rps = h;
    const int bps = int(b / 8);
    constexpr uint32_t kMaxSide = uint32_t(std::numeric_limits<int>::max() / 6); // w * 3 samples * 2 bytes fits an int
    if (w > kMaxSide || h > kMaxSide)
        return fail(QObject::tr("Image too large (%1 x %2)").arg(w).arg(h));
    // all in 64 bit: w, h < 2^29 and spp * bps <= 8, so nothing below can overflow
    const uint64_t rowBytes = uint64_t(w) * spp * bps;
    const uint64_t pixels = uint64_t(w) * h;
    if (pixels > uint64_t(std::numeric_limits<size_t>::max() / 3 / sizeof(uint16_t)))
        return fail(QObject::tr("Image too large (%1 x %2)").arg(w).arg(h));
    // the strips must be able to hold the rows: uncompressed exactly, Deflate at
    // most ~1032:1 (zlib's limit)
    uint64_t stripTotal = 0;
    for (uint32_t c : cnts)
        stripTotal += c;
    const uint64_t rawTotal = rowBytes * h;
    if (comp == 1 ? stripTotal < rawTotal : stripTotal * 1100 < rawTotal)
        return fail(QObject::tr("Truncated TIFF"));
    // one strip is inflated into one QByteArray, whose expected size is 32 bit
    if (rowBytes * rps > uint64_t(std::numeric_limits<int>::max()))
        return fail(QObject::tr("Unsupported TIFF layout (strips of %1 rows are too large)").arg(rps));
    bits = int(b);
    // Only a physical calibration counts: unit "none" is ignored, and so are
    // resolutions below 20000 px/m (> 50 um/px), the same threshold as the PNG
    // path. This also rejects the 72 dpi default written by older versions for
    // uncalibrated images (which would otherwise read back as 352.8 um/px).
    umPerPixel = 0;
    const double pxPerMetre = resUnit == 3 ? xres * 100.0 : resUnit == 2 ? xres / 0.0254 : 0.0;
    if (pxPerMetre > kMinCalibratedPxPerMetre)
        umPerPixel = 1e6 / pxPerMetre;

    try {
        img = Image16(int(w), int(h));
        std::vector<uint32_t> samples(size_t(w) * spp);
        uint32_t y = 0;
        for (size_t s = 0; s < offs.size() && y < h; ++s) {
            if (qsizetype(offs[s]) + qsizetype(cnts[s]) > n)
                return fail(QObject::tr("Truncated TIFF"));
            QByteArray chunk(reinterpret_cast<const char *>(d + offs[s]), qsizetype(cnts[s]));
            const uint32_t rows = std::min(rps, h - y);
            const size_t stripBytes = size_t(rowBytes) * rows;
            if (comp != 1)
                chunk = inflate(chunk, int(stripBytes));
            if (size_t(chunk.size()) < stripBytes)
                return fail(QObject::tr("Corrupt TIFF strip"));
            const uchar *p = reinterpret_cast<const uchar *>(chunk.constData());
            for (uint32_t r = 0; r < rows; ++r, ++y) {
                const uchar *row = p + size_t(rowBytes) * r;
                for (size_t i = 0; i < samples.size(); ++i)
                    samples[i] = bps == 2 ? uint32_t(le ? row[2 * i] | row[2 * i + 1] << 8 : row[2 * i] << 8 | row[2 * i + 1])
                                          : row[i];
                if (predictor == 2)
                    for (size_t i = spp; i < samples.size(); ++i)
                        samples[i] = (samples[i] + samples[i - spp]) & (bps == 2 ? 0xFFFF : 0xFF);
                uint16_t *o = img.row(int(y));
                for (uint32_t x = 0; x < w; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        uint32_t v = samples[size_t(x) * spp + (spp >= 3 ? c : 0)];
                        if (photometric == 0)
                            v = (bps == 2 ? 0xFFFF : 0xFF) - v;
                        o[size_t(x) * 3 + c] = uint16_t(bps == 2 ? v : v * 257);
                    }
                }
            }
        }
    } catch (const std::bad_alloc &) {
        img = Image16();
        return fail(QObject::tr("Not enough memory to open a %1 x %2 image").arg(w).arg(h));
    }
    return true;
}

// ============================================================ helpers
QString extensionFor(FileFormat f)
{
    switch (f) {
    case FileFormat::Tiff: return QStringLiteral("tif");
    case FileFormat::Png: return QStringLiteral("png");
    case FileFormat::Jpeg: return QStringLiteral("jpg");
    case FileFormat::Bmp: return QStringLiteral("bmp");
    }
    return QStringLiteral("tif");
}

FileFormat formatFromExtension(const QString &path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    if (s == QLatin1String("png")) return FileFormat::Png;
    if (s == QLatin1String("jpg") || s == QLatin1String("jpeg")) return FileFormat::Jpeg;
    if (s == QLatin1String("bmp")) return FileFormat::Bmp;
    return FileFormat::Tiff;
}

QImage toQImage8(const Image16 &img)
{
    QImage q(img.width, img.height, QImage::Format_RGB888);
    for (int y = 0; y < img.height; ++y) {
        const uint16_t *s = img.row(y);
        uchar *d = q.scanLine(y);
        for (int i = 0; i < img.width * 3; ++i)
            d[i] = uchar((s[i] + 128) / 257);
    }
    return q;
}

QImage toQImage8(const Image8 &img)
{
    QImage q(img.width, img.height, QImage::Format_RGB888);
    for (int y = 0; y < img.height; ++y)
        std::memcpy(q.scanLine(y), img.row(y), size_t(img.width) * 3);
    return q;
}

Image16 fromQImage(const QImage &src)
{
    const bool deep = src.depth() > 32;
    QImage img = src.convertToFormat(deep ? QImage::Format_RGBX64 : QImage::Format_RGB888);
    Image16 out(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y) {
        uint16_t *o = out.row(y);
        if (deep) {
            const quint16 *s = reinterpret_cast<const quint16 *>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                o[x * 3] = s[x * 4];
                o[x * 3 + 1] = s[x * 4 + 1];
                o[x * 3 + 2] = s[x * 4 + 2];
            }
        } else {
            const uchar *s = img.constScanLine(y);
            for (int i = 0; i < img.width() * 3; ++i)
                o[i] = uint16_t(s[i] * 257);
        }
    }
    return out;
}

QString sidecarPath(const QString &imagePath)
{
    return imagePath + QStringLiteral(".json");
}

static bool writeSidecar(const QString &path, const ImageMetadata &meta)
{
    QSaveFile f(sidecarPath(path));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(meta.toJson()).toJson(QJsonDocument::Indented));
    return f.commit();
}

bool saveImage(const QString &path, const Image16 &img, const ImageMetadata &metaIn, const SaveOptions &opt,
               QString *error)
{
    ImageMetadata meta = metaIn;
    meta.width = img.width;
    meta.height = img.height;
    switch (opt.format) {
    case FileFormat::Tiff:
        meta.bitDepth = opt.sixteenBit ? 16 : 8;
        return writeTiff(path, img, meta.bitDepth, opt.compress, meta.umPerPixel, meta.toJsonString(), error);
    case FileFormat::Png: {
        QImage q;
        if (opt.sixteenBit) {
            q = QImage(img.width, img.height, QImage::Format_RGBX64);
            for (int y = 0; y < img.height; ++y) {
                quint16 *d = reinterpret_cast<quint16 *>(q.scanLine(y));
                const uint16_t *s = img.row(y);
                for (int x = 0; x < img.width; ++x) {
                    d[x * 4] = s[x * 3];
                    d[x * 4 + 1] = s[x * 3 + 1];
                    d[x * 4 + 2] = s[x * 3 + 2];
                    d[x * 4 + 3] = 0xFFFF;
                }
            }
            meta.bitDepth = 16;
        } else {
            q = toQImage8(img);
            meta.bitDepth = 8;
        }
        return saveImage(path, q, meta, opt, error);
    }
    default:
        meta.bitDepth = 8;
        return saveImage(path, toQImage8(img), meta, opt, error);
    }
}

bool saveImage(const QString &path, const QImage &imgIn, const ImageMetadata &metaIn, const SaveOptions &opt,
               QString *error)
{
    ImageMetadata meta = metaIn;
    meta.width = imgIn.width();
    meta.height = imgIn.height();
    if (opt.format == FileFormat::Tiff) {
        Image16 d = fromQImage(imgIn);
        meta.bitDepth = imgIn.depth() > 32 && opt.sixteenBit ? 16 : 8;
        return writeTiff(path, d, meta.bitDepth, opt.compress, meta.umPerPixel, meta.toJsonString(), error);
    }
    QImage img = imgIn;
    if (meta.umPerPixel > 0) {
        const int dpm = int(std::lround(1e6 / meta.umPerPixel));
        img.setDotsPerMeterX(dpm);
        img.setDotsPerMeterY(dpm);
    }
    img.setColorSpace(QColorSpace::SRgb);
    QSaveFile out(path); // atomic: the target is replaced only after a complete write
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = out.errorString();
        return false;
    }
    QImageWriter w(&out, extensionFor(opt.format).toLatin1());
    if (opt.format == FileFormat::Jpeg) {
        w.setQuality(opt.jpegQuality);
        w.setOptimizedWrite(true);
        img = img.convertToFormat(QImage::Format_RGB888);
    }
    if (opt.format == FileFormat::Png)
        w.setCompression(6);
    w.setText(QStringLiteral("Description"), meta.toJsonString());
    w.setText(QStringLiteral("Software"), meta.software);
    if (!w.write(img)) {
        if (error) *error = w.errorString();
        out.cancelWriting();
        return false;
    }
    if (!out.commit()) {
        if (error) *error = out.errorString();
        return false;
    }
    if (opt.writeSidecar && !writeSidecar(path, meta)) {
        if (error) *error = QObject::tr("The image was saved, but its metadata file could not be written.");
        return false;
    }
    return true;
}

static bool loadImageImpl(const QString &path, LoadedImage &out, QString *error);

bool loadImage(const QString &path, LoadedImage &out, QString *error)
{
    try {
        // A Leica .lif is a container of many images; this reads the first one.
        // The user interface offers the whole list (see ui/LifDialog.h).
        if (QFileInfo(path).suffix().compare(QLatin1String("lif"), Qt::CaseInsensitive) == 0) {
            QList<LifEntry> entries;
            if (readLifIndex(path, entries, error) && readLifImage(path, entries.first(), out, error))
                return true;
        } else if (loadImageImpl(path, out, error)) {
            return true;
        }
    } catch (const std::bad_alloc &) {
        if (error) *error = QObject::tr("Not enough memory to open the image");
    } catch (const std::exception &e) {
        if (error) *error = QString::fromUtf8(e.what());
    }
    out = LoadedImage{};
    return false;
}

namespace {

// The sidecar's pixel size replaces the embedded one (see ImageIO.h).
void takeCalibration(const ImageMetadata &from, ImageMetadata &to)
{
    to.umPerPixel = from.umPerPixel;
    to.adapterFactor = from.adapterFactor;
    to.pixelSizeSource = from.pixelSizeSource;
}

} // namespace

static bool loadImageImpl(const QString &path, LoadedImage &out, QString *error)
{
    out = LoadedImage{};
    out.path = path;
    const FileFormat fmt = formatFromExtension(path);
    ImageMetadata sidecar;
    const bool haveSidecar = loadSidecarMetadata(path, sidecar);
    const auto addSidecar = [&] {
        if (!haveSidecar)
            return;
        if (out.hasMeta) {
            takeCalibration(sidecar, out.meta);
        } else {
            out.meta = sidecar;
            out.hasMeta = true;
        }
    };
    QString desc;
    if (fmt == FileFormat::Tiff) {
        int bits = 8;
        double um = 0;
        QString err;
        if (readTiff(path, out.data, bits, desc, um, &err)) {
            out.sourceBitDepth = bits;
            if (!desc.isEmpty() && ImageMetadata::fromJsonString(desc, out.meta))
                out.hasMeta = true;
            addSidecar();
            // DM Imaging metadata is authoritative, including 0 = uncalibrated;
            // the resolution tag is only used for other files
            const bool jsonHasPixelSize =
                haveSidecar
                || (out.hasMeta && QJsonDocument::fromJson(desc.toUtf8()).object().contains(QLatin1String("umPerPixel")));
            if (!jsonHasPixelSize && um > 0 && out.meta.umPerPixel <= 0)
                out.meta.umPerPixel = um;
            return true;
        }
        // fall through to Qt for TIFF variants we do not decode
    }
    QImageReader r(path);
    r.setAutoTransform(true);
    QImage img = r.read();
    if (img.isNull()) {
        if (error) *error = r.errorString();
        return false;
    }
    out.sourceBitDepth = img.depth() > 32 ? 16 : 8;
    out.data = fromQImage(img);
    // the decoded image carries every text chunk; the reader may only have the
    // ones before the pixel data
    desc = img.text(QStringLiteral("Description"));
    if (desc.isEmpty())
        desc = r.text(QStringLiteral("Description"));
    if (!desc.isEmpty() && ImageMetadata::fromJsonString(desc, out.meta))
        out.hasMeta = true;
    addSidecar();
    if (out.meta.umPerPixel <= 0 && img.dotsPerMeterX() > kMinCalibratedPxPerMetre) // > 20 px/mm: physical calibration
        out.meta.umPerPixel = 1e6 / img.dotsPerMeterX();
    return true;
}

bool loadSidecarMetadata(const QString &path, ImageMetadata &out)
{
    QFile sc(sidecarPath(path));
    if (!sc.open(QIODevice::ReadOnly))
        return false;
    const auto doc = QJsonDocument::fromJson(sc.readAll());
    if (!doc.isObject())
        return false;
    out = ImageMetadata::fromJson(doc.object());
    return true;
}

bool loadEmbeddedMetadata(const QString &path, ImageMetadata &out)
{
    if (formatFromExtension(path) == FileFormat::Tiff) {
        // parse only the IFD for the description
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        const QByteArray head = f.read(8);
        if (head.size() < 8 || head[0] != 'I')
            return false;
        const uchar *h = reinterpret_cast<const uchar *>(head.constData());
        const quint32 ifd = h[4] | h[5] << 8 | h[6] << 16 | quint32(h[7]) << 24;
        if (!f.seek(ifd))
            return false;
        const QByteArray cntB = f.read(2);
        if (cntB.size() < 2)
            return false;
        const int cnt = uchar(cntB[0]) | uchar(cntB[1]) << 8;
        const QByteArray entries = f.read(12 * cnt);
        for (int i = 0; i + 12 <= entries.size(); i += 12) {
            const uchar *e = reinterpret_cast<const uchar *>(entries.constData() + i);
            if ((e[0] | e[1] << 8) == kDescription) {
                const quint32 len = e[4] | e[5] << 8 | e[6] << 16 | quint32(e[7]) << 24;
                const quint32 off = e[8] | e[9] << 8 | e[10] << 16 | quint32(e[11]) << 24;
                if (len > 4 && len < 10 * 1024 * 1024 && f.seek(off)) {
                    const QString s = QString::fromUtf8(f.read(len)).remove(QChar(0));
                    return ImageMetadata::fromJsonString(s, out);
                }
            }
        }
        return false;
    }
    QImageReader r(path);
    const QString desc = r.text(QStringLiteral("Description"));
    return !desc.isEmpty() && ImageMetadata::fromJsonString(desc, out);
}

bool loadMetadata(const QString &path, ImageMetadata &out)
{
    if (QFileInfo(path).suffix().compare(QLatin1String("lif"), Qt::CaseInsensitive) == 0) {
        QList<LifEntry> entries;
        if (!readLifIndex(path, entries, nullptr))
            return false;
        const LifEntry &e = entries.first();
        out = ImageMetadata();
        out.umPerPixel = e.umPerPixel;
        out.width = e.width;
        out.height = e.height;
        out.bitDepth = e.bitsPerSample;
        out.sample = e.name;
        out.software = QStringLiteral("Leica LAS X (.lif)");
        return true;
    }

    // sidecar first (cheap, and its pixel size wins anyway), then embedded text
    return loadSidecarMetadata(path, out) || loadEmbeddedMetadata(path, out);
}

} // namespace lm
