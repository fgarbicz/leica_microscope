#include "LifFile.h"

#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cstring>

namespace lm {

namespace {

constexpr qint32 kTestCode = 0x70;
constexpr quint8 kMarker = 0x2A;

// --- little endian readers over an open file -------------------------------
bool readExact(QFile &f, void *dst, qint64 n)
{
    return f.read(static_cast<char *>(dst), n) == n;
}
bool readU8(QFile &f, quint8 &v) { return readExact(f, &v, 1); }
bool readU32(QFile &f, quint32 &v)
{
    uchar b[4];
    if (!readExact(f, b, 4))
        return false;
    v = quint32(b[0]) | quint32(b[1]) << 8 | quint32(b[2]) << 16 | quint32(b[3]) << 24;
    return true;
}
bool readI32(QFile &f, qint32 &v)
{
    quint32 u = 0;
    if (!readU32(f, u))
        return false;
    v = qint32(u);
    return true;
}
bool readU64(QFile &f, quint64 &v)
{
    uchar b[8];
    if (!readExact(f, b, 8))
        return false;
    v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | b[i];
    return true;
}
// UTF-16LE string of `chars` code units
bool readUtf16(QFile &f, quint32 chars, QString &out)
{
    if (chars > (1u << 24)) // a name that long means the file is not what we think
        return false;
    QByteArray raw = f.read(qint64(chars) * 2);
    if (raw.size() != qint64(chars) * 2)
        return false;
    out = QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData()), int(chars));
    return true;
}

// --- little endian writers -------------------------------------------------
void writeU32(QFile &f, quint32 v)
{
    const uchar b[4] = {uchar(v), uchar(v >> 8), uchar(v >> 16), uchar(v >> 24)};
    f.write(reinterpret_cast<const char *>(b), 4);
}
void writeU64(QFile &f, quint64 v)
{
    uchar b[8];
    for (int i = 0; i < 8; ++i)
        b[i] = uchar(v >> (8 * i));
    f.write(reinterpret_cast<const char *>(b), 8);
}
void writeUtf16(QFile &f, const QString &s)
{
    for (const QChar c : s) {
        const ushort u = c.unicode();
        const uchar b[2] = {uchar(u & 0xff), uchar(u >> 8)};
        f.write(reinterpret_cast<const char *>(b), 2);
    }
}

// Length values in the XML are metres, written in the C locale.
double metresToUmPerPixel(const QString &length, int elements)
{
    bool ok = false;
    const double metres = length.toDouble(&ok);
    if (!ok || elements <= 0 || metres <= 0)
        return 0;
    return metres * 1e6 / elements;
}

struct XmlEntry {
    QString name;
    QString path;
    QString blockId;
    int width = 0, height = 0;
    int bits = 8;
    double umPerPixel = 0;
    int redOffset = 2, greenOffset = 1, blueOffset = 0;
    int channels = 0;
    qint64 rowStride = 0;
    qint64 bytesPerPixel = 0;
};

// Walks Element/Children/Element, collecting the ones that carry an image.
void parseElement(QXmlStreamReader &r, const QString &parentPath, QList<XmlEntry> &out)
{
    XmlEntry e;
    e.name = r.attributes().value(QStringLiteral("Name")).toString();
    const QString here = parentPath.isEmpty() ? e.name : parentPath + QLatin1Char('/') + e.name;
    e.path = parentPath;
    bool isImage = false;

    while (!r.atEnd()) {
        r.readNext();
        if (r.isEndElement() && r.name() == QLatin1String("Element"))
            break;
        if (!r.isStartElement())
            continue;
        const QStringView tag = r.name();
        if (tag == QLatin1String("Element")) {
            parseElement(r, here, out); // a folder inside a folder
        } else if (tag == QLatin1String("Image")) {
            isImage = true;
        } else if (tag == QLatin1String("Memory")) {
            e.blockId = r.attributes().value(QStringLiteral("MemoryBlockID")).toString();
        } else if (tag == QLatin1String("ChannelDescription")) {
            const auto a = r.attributes();
            const int inc = a.value(QStringLiteral("BytesInc")).toInt();
            const int tagId = a.value(QStringLiteral("ChannelTag")).toInt();
            e.bits = std::max(e.bits, a.value(QStringLiteral("Resolution")).toInt());
            // ChannelTag 1 red, 2 green, 3 blue (LUTName says the same)
            const QString lut = a.value(QStringLiteral("LUTName")).toString();
            if (tagId == 1 || lut.compare(QLatin1String("Red"), Qt::CaseInsensitive) == 0)
                e.redOffset = inc;
            else if (tagId == 2 || lut.compare(QLatin1String("Green"), Qt::CaseInsensitive) == 0)
                e.greenOffset = inc;
            else if (tagId == 3 || lut.compare(QLatin1String("Blue"), Qt::CaseInsensitive) == 0)
                e.blueOffset = inc;
            ++e.channels;
        } else if (tag == QLatin1String("DimensionDescription")) {
            const auto a = r.attributes();
            const QString dim = a.value(QStringLiteral("DimID")).toString();
            const int n = a.value(QStringLiteral("NumberOfElements")).toInt();
            const QString len = a.value(QStringLiteral("Length")).toString();
            const qint64 inc = a.value(QStringLiteral("BytesInc")).toLongLong();
            // DimID is "X"/"Y" in the sidecar XML and "1"/"2" inside the file
            if (dim == QLatin1String("X") || dim == QLatin1String("1")) {
                e.width = n;
                e.bytesPerPixel = inc;
                if (e.umPerPixel <= 0)
                    e.umPerPixel = metresToUmPerPixel(len, n);
            } else if (dim == QLatin1String("Y") || dim == QLatin1String("2")) {
                e.height = n;
                e.rowStride = inc;
            }
            // higher dimensions (z, t) are not read: this camera is 2D
        }
    }
    if (isImage && e.width > 0 && e.height > 0 && !e.blockId.isEmpty())
        out.push_back(e);
}

// The header, and the offset just past it.
bool readHeader(QFile &f, QString &xml, QString *error)
{
    qint32 testCode = 0;
    quint32 headerSize = 0, chars = 0;
    quint8 marker = 0;
    if (!readI32(f, testCode) || testCode != kTestCode) {
        if (error)
            *error = QObject::tr("Not a Leica .lif file (bad file marker).");
        return false;
    }
    if (!readU32(f, headerSize) || !readU8(f, marker) || marker != kMarker || !readU32(f, chars)) {
        if (error)
            *error = QObject::tr("The .lif header is damaged.");
        return false;
    }
    if (!readUtf16(f, chars, xml)) {
        if (error)
            *error = QObject::tr("The .lif header ends unexpectedly.");
        return false;
    }
    return true;
}

struct Block {
    QString id;
    qint64 offset = 0;
    qint64 size = 0;
};

// Walks the memory blocks after the header. `wide` selects the 64-bit size
// field of version 2; version 1 files use 32 bits.
bool readBlocks(QFile &f, bool wide, QList<Block> &out)
{
    while (!f.atEnd()) {
        qint32 testCode = 0;
        if (!readI32(f, testCode))
            break; // trailing padding
        if (testCode != kTestCode)
            return false;
        quint32 descSize = 0, chars = 0;
        quint8 m1 = 0, m2 = 0;
        quint64 memSize = 0;
        if (!readU32(f, descSize) || !readU8(f, m1) || m1 != kMarker)
            return false;
        if (wide) {
            if (!readU64(f, memSize))
                return false;
        } else {
            quint32 s = 0;
            if (!readU32(f, s))
                return false;
            memSize = s;
        }
        QString id;
        if (!readU8(f, m2) || m2 != kMarker || !readU32(f, chars) || !readUtf16(f, chars, id))
            return false;
        Block b;
        b.id = id;
        b.offset = f.pos();
        b.size = qint64(memSize);
        out.push_back(b);
        if (!f.seek(f.pos() + b.size))
            return false;
    }
    return true;
}

} // namespace

bool readLifIndex(const QString &path, QList<LifEntry> &out, QString *error)
{
    out.clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot open %1: %2").arg(QFileInfo(path).fileName(), f.errorString());
        return false;
    }
    QString xml;
    if (!readHeader(f, xml, error))
        return false;

    int version = 2;
    QList<XmlEntry> entries;
    {
        QXmlStreamReader r(xml);
        while (!r.atEnd()) {
            r.readNext();
            if (!r.isStartElement())
                continue;
            if (r.name() == QLatin1String("LMSDataContainerHeader")) {
                const int v = r.attributes().value(QStringLiteral("Version")).toInt();
                if (v > 0)
                    version = v;
            } else if (r.name() == QLatin1String("Element")) {
                parseElement(r, QString(), entries);
            }
        }
        if (r.hasError()) {
            if (error)
                *error = QObject::tr("The .lif index is not valid XML: %1").arg(r.errorString());
            return false;
        }
    }

    QList<Block> blocks;
    if (!readBlocks(f, version >= 2, blocks)) {
        if (error)
            *error = QObject::tr("The .lif image data is damaged.");
        return false;
    }

    for (const XmlEntry &e : entries) {
        const auto it = std::find_if(blocks.begin(), blocks.end(), [&](const Block &b) { return b.id == e.blockId; });
        if (it == blocks.end() || it->size <= 0)
            continue; // the XML names a block the file does not contain
        LifEntry le;
        le.name = e.name;
        le.path = e.path;
        le.width = e.width;
        le.height = e.height;
        le.channels = std::max(1, e.channels);
        le.bitsPerSample = e.bits >= 9 ? 16 : 8;
        le.umPerPixel = e.umPerPixel;
        le.blockId = e.blockId;
        le.dataOffset = it->offset;
        le.dataBytes = it->size;
        le.redOffset = e.redOffset;
        le.greenOffset = e.greenOffset;
        le.blueOffset = e.blueOffset;
        const int sampleBytes = le.bitsPerSample / 8;
        le.bytesPerPixel = int(e.bytesPerPixel > 0 ? e.bytesPerPixel : qint64(le.channels) * sampleBytes);
        le.rowStride = e.rowStride > 0 ? e.rowStride : qint64(le.width) * le.bytesPerPixel;
        out.push_back(le);
    }
    if (out.isEmpty() && error)
        *error = QObject::tr("%1 holds no images this program can read.").arg(QFileInfo(path).fileName());
    return !out.isEmpty();
}

bool readLifImage(const QString &path, const LifEntry &e, LoadedImage &out, QString *error)
{
    if (e.width <= 0 || e.height <= 0) {
        if (error)
            *error = QObject::tr("The image has no size.");
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot open %1: %2").arg(QFileInfo(path).fileName(), f.errorString());
        return false;
    }
    const qint64 need = e.rowStride * e.height;
    if (e.dataBytes < need) {
        if (error)
            *error = QObject::tr("The image data is shorter than its size says (%1 of %2 bytes).")
                         .arg(e.dataBytes)
                         .arg(need);
        return false;
    }
    if (!f.seek(e.dataOffset)) {
        if (error)
            *error = QObject::tr("Cannot reach the image data.");
        return false;
    }

    Image16 img(e.width, e.height);
    if (img.px.empty()) {
        if (error)
            *error = QObject::tr("Not enough memory for a %1 x %2 image.").arg(e.width).arg(e.height);
        return false;
    }
    const int sampleBytes = e.bitsPerSample / 8;
    const bool mono = e.channels < 3;
    QByteArray row;
    for (int y = 0; y < e.height; ++y) {
        row = f.read(e.rowStride);
        if (row.size() != e.rowStride) {
            if (error)
                *error = QObject::tr("The image data ends at row %1 of %2.").arg(y).arg(e.height);
            return false;
        }
        const uchar *src = reinterpret_cast<const uchar *>(row.constData());
        uint16_t *dst = img.row(y);
        for (int x = 0; x < e.width; ++x) {
            const uchar *p = src + qint64(x) * e.bytesPerPixel;
            auto sample = [&](int offset) -> uint16_t {
                if (sampleBytes == 2) {
                    const uchar *s = p + offset;
                    return uint16_t(quint16(s[0]) | quint16(s[1]) << 8);
                }
                // 8-bit sources are scaled the same way as everywhere else (x257)
                return uint16_t(p[offset] * 257);
            };
            if (mono) {
                const uint16_t v = sample(0);
                dst[3 * x] = dst[3 * x + 1] = dst[3 * x + 2] = v;
            } else {
                dst[3 * x] = sample(e.redOffset * sampleBytes);
                dst[3 * x + 1] = sample(e.greenOffset * sampleBytes);
                dst[3 * x + 2] = sample(e.blueOffset * sampleBytes);
            }
        }
    }

    out.data = std::move(img);
    out.sourceBitDepth = e.bitsPerSample;
    out.path = path;
    out.hasMeta = e.umPerPixel > 0;
    out.meta = ImageMetadata();
    out.meta.umPerPixel = e.umPerPixel;
    out.meta.width = e.width;
    out.meta.height = e.height;
    out.meta.bitDepth = e.bitsPerSample;
    out.meta.sample = e.name; // the name LAS X gave the image
    out.meta.software = QStringLiteral("Leica LAS X (.lif)");
    return true;
}

bool writeLif(const QString &path, const QList<LifImageOut> &images, const QString &experimentName, QString *error)
{
    if (images.isEmpty()) {
        if (error)
            *error = QObject::tr("There are no images to write.");
        return false;
    }
    for (const LifImageOut &im : images) {
        if (im.data.empty()) {
            if (error)
                *error = QObject::tr("\"%1\" has no image data.").arg(im.name);
            return false;
        }
    }

    // Block ids in the same style LAS X uses, so the file reads naturally in it.
    QStringList blockIds;
    for (int i = 0; i < images.size(); ++i)
        blockIds << QStringLiteral("MemBlock_%1").arg(i + 1);

    const QString title = experimentName.isEmpty() ? QFileInfo(path).fileName() : experimentName;

    QString xml;
    {
        QXmlStreamWriter w(&xml);
        w.writeStartElement(QStringLiteral("LMSDataContainerHeader"));
        w.writeAttribute(QStringLiteral("Version"), QStringLiteral("2"));
        w.writeStartElement(QStringLiteral("Element"));
        w.writeAttribute(QStringLiteral("Name"), title);
        w.writeAttribute(QStringLiteral("Visibility"), QStringLiteral("1"));
        w.writeAttribute(QStringLiteral("CopyOption"), QStringLiteral("1"));
        w.writeAttribute(QStringLiteral("UniqueID"),
                         QUuid::createUuid().toString(QUuid::WithoutBraces));
        w.writeStartElement(QStringLiteral("Data"));
        w.writeEndElement();
        // the top level element owns no pixels
        w.writeStartElement(QStringLiteral("Memory"));
        w.writeAttribute(QStringLiteral("Size"), QStringLiteral("0"));
        w.writeAttribute(QStringLiteral("MemoryBlockID"), QStringLiteral("MemBlock_0"));
        w.writeEndElement();
        w.writeStartElement(QStringLiteral("Children"));

        for (int i = 0; i < images.size(); ++i) {
            const LifImageOut &im = images[i];
            const int bits = im.eightBit ? 8 : 16;
            const int sampleBytes = bits / 8;
            const qint64 bpp = qint64(3) * sampleBytes;
            const qint64 stride = bpp * im.data.width;
            const qint64 bytes = stride * im.data.height;

            w.writeStartElement(QStringLiteral("Element"));
            w.writeAttribute(QStringLiteral("Name"), im.name);
            w.writeAttribute(QStringLiteral("Visibility"), QStringLiteral("1"));
            w.writeAttribute(QStringLiteral("CopyOption"), QStringLiteral("1"));
            w.writeAttribute(QStringLiteral("UniqueID"), QUuid::createUuid().toString(QUuid::WithoutBraces));

            w.writeStartElement(QStringLiteral("Data"));
            w.writeStartElement(QStringLiteral("Image"));
            w.writeAttribute(QStringLiteral("TextDescription"), QString());
            w.writeStartElement(QStringLiteral("ImageDescription"));

            w.writeStartElement(QStringLiteral("Channels"));
            // interleaved BGR, the order a Leica camera writes
            const struct { const char *lut; int tag; int inc; } kChannels[] = {
                {"Blue", 3, 0}, {"Green", 2, 1}, {"Red", 1, 2}};
            for (const auto &c : kChannels) {
                w.writeStartElement(QStringLiteral("ChannelDescription"));
                w.writeAttribute(QStringLiteral("DataType"), QStringLiteral("0"));
                w.writeAttribute(QStringLiteral("ChannelTag"), QString::number(c.tag));
                w.writeAttribute(QStringLiteral("Resolution"), QString::number(bits));
                w.writeAttribute(QStringLiteral("NameOfMeasuredQuantity"), QString());
                w.writeAttribute(QStringLiteral("Min"), QStringLiteral("0.000000e+000"));
                w.writeAttribute(QStringLiteral("Max"),
                                 bits == 8 ? QStringLiteral("2.550000e+002") : QStringLiteral("6.553500e+004"));
                w.writeAttribute(QStringLiteral("Unit"), QString());
                w.writeAttribute(QStringLiteral("LUTName"), QLatin1String(c.lut));
                w.writeAttribute(QStringLiteral("IsLUTInverted"), QStringLiteral("0"));
                w.writeAttribute(QStringLiteral("BytesInc"), QString::number(c.inc * sampleBytes));
                w.writeAttribute(QStringLiteral("BitInc"), QStringLiteral("0"));
                w.writeEndElement();
            }
            w.writeEndElement(); // Channels

            w.writeStartElement(QStringLiteral("Dimensions"));
            const auto writeDim = [&](int id, int n, qint64 inc) {
                // Length is the physical size in metres; 0 when uncalibrated
                const double metres = im.umPerPixel > 0 ? im.umPerPixel * 1e-6 * n : 0.0;
                w.writeStartElement(QStringLiteral("DimensionDescription"));
                w.writeAttribute(QStringLiteral("DimID"), QString::number(id));
                w.writeAttribute(QStringLiteral("NumberOfElements"), QString::number(n));
                w.writeAttribute(QStringLiteral("Origin"), QStringLiteral("0.000000e+000"));
                w.writeAttribute(QStringLiteral("Length"), QString::asprintf("%.6e", metres));
                w.writeAttribute(QStringLiteral("Unit"), QStringLiteral("m"));
                w.writeAttribute(QStringLiteral("BitInc"), QStringLiteral("0"));
                w.writeAttribute(QStringLiteral("BytesInc"), QString::number(inc));
                w.writeEndElement();
            };
            writeDim(1, im.data.width, bpp);
            writeDim(2, im.data.height, stride);
            w.writeEndElement(); // Dimensions

            w.writeEndElement(); // ImageDescription
            w.writeEndElement(); // Image
            w.writeEndElement(); // Data

            w.writeStartElement(QStringLiteral("Memory"));
            w.writeAttribute(QStringLiteral("Size"), QString::number(bytes));
            w.writeAttribute(QStringLiteral("MemoryBlockID"), blockIds[i]);
            w.writeEndElement();

            w.writeEndElement(); // Element
        }
        w.writeEndElement(); // Children
        w.writeEndElement(); // Element
        w.writeEndElement(); // LMSDataContainerHeader
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2").arg(QFileInfo(path).fileName(), f.errorString());
        return false;
    }

    // header: the size field counts everything after it
    const quint32 chars = quint32(xml.size());
    writeU32(f, quint32(kTestCode));
    writeU32(f, quint32(1 + 4 + chars * 2));
    const uchar marker = kMarker;
    f.write(reinterpret_cast<const char *>(&marker), 1);
    writeU32(f, chars);
    writeUtf16(f, xml);

    // the top level element's empty block, then one block per image
    const auto writeBlockHead = [&](const QString &id, quint64 bytes) {
        writeU32(f, quint32(kTestCode));
        writeU32(f, quint32(1 + 8 + 1 + 4 + quint32(id.size()) * 2));
        f.write(reinterpret_cast<const char *>(&marker), 1);
        writeU64(f, bytes);
        f.write(reinterpret_cast<const char *>(&marker), 1);
        writeU32(f, quint32(id.size()));
        writeUtf16(f, id);
    };
    writeBlockHead(QStringLiteral("MemBlock_0"), 0);

    for (int i = 0; i < images.size(); ++i) {
        const LifImageOut &im = images[i];
        const int sampleBytes = im.eightBit ? 1 : 2;
        const qint64 stride = qint64(3) * sampleBytes * im.data.width;
        writeBlockHead(blockIds[i], quint64(stride * im.data.height));
        QByteArray row(stride, Qt::Uninitialized);
        for (int y = 0; y < im.data.height; ++y) {
            uchar *dst = reinterpret_cast<uchar *>(row.data());
            const uint16_t *src = im.data.row(y);
            for (int x = 0; x < im.data.width; ++x) {
                const uint16_t r = src[3 * x], g = src[3 * x + 1], b = src[3 * x + 2];
                if (sampleBytes == 1) {
                    dst[3 * x] = uchar(b >> 8);      // B, G, R as LAS X stores them
                    dst[3 * x + 1] = uchar(g >> 8);
                    dst[3 * x + 2] = uchar(r >> 8);
                } else {
                    const uint16_t v[3] = {b, g, r};
                    for (int c = 0; c < 3; ++c) {
                        dst[6 * x + 2 * c] = uchar(v[c] & 0xff);
                        dst[6 * x + 2 * c + 1] = uchar(v[c] >> 8);
                    }
                }
            }
            if (f.write(row) != stride) {
                if (error)
                    *error = QObject::tr("Writing %1 failed: %2").arg(QFileInfo(path).fileName(), f.errorString());
                f.remove();
                return false;
            }
        }
    }
    if (!f.flush()) {
        if (error)
            *error = QObject::tr("Writing %1 failed: %2").arg(QFileInfo(path).fileName(), f.errorString());
        f.remove();
        return false;
    }
    return true;
}

} // namespace lm
