#include "LifFile.h"

#include "core/Parallel.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPoint>
#include <QSaveFile>
#include <QSize>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace lm {

namespace {

constexpr qint32 kTestCode = 0x70;
constexpr quint8 kMarker = 0x2A;
// Sanity limits for what the XML may claim, checked before anything is
// allocated: an image side must fit Image16's int arithmetic (w * 3 samples *
// 2 bytes), and no image has more channels than this (a spectral scan has a
// few dozen).
constexpr qint64 kMaxSide = std::numeric_limits<int>::max() / 6;
constexpr int kMaxChannels = 256;
constexpr int kMaxDims = 16;
// Rows are read in chunks of about this many bytes: few reads, little memory.
constexpr qint64 kChunkBytes = 16 * 1024 * 1024;

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
    if (chars > (1u << 28)) // a header that long means the file is not what we think
        return false;
    QByteArray raw = f.read(qint64(chars) * 2);
    if (raw.size() != qint64(chars) * 2)
        return false;
    out = QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData()), int(chars));
    return true;
}

// --- little endian writers -------------------------------------------------
void writeU32(QIODevice &f, quint32 v)
{
    const uchar b[4] = {uchar(v), uchar(v >> 8), uchar(v >> 16), uchar(v >> 24)};
    f.write(reinterpret_cast<const char *>(b), 4);
}
void writeU64(QIODevice &f, quint64 v)
{
    uchar b[8];
    for (int i = 0; i < 8; ++i)
        b[i] = uchar(v >> (8 * i));
    f.write(reinterpret_cast<const char *>(b), 8);
}
void writeUtf16(QIODevice &f, const QString &s)
{
    for (const QChar c : s) {
        const ushort u = c.unicode();
        const uchar b[2] = {uchar(u & 0xff), uchar(u >> 8)};
        f.write(reinterpret_cast<const char *>(b), 2);
    }
}

// Numbers in the XML are written in the C locale.
double number(const QStringView s)
{
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok && std::isfinite(v) ? v : 0.0;
}

// What a LUT shows at full intensity. Names LAS X uses; anything else
// (Gray, "P.Color 4", Fire…) is shown in white.
QRgb lutColour(const QString &lut)
{
    static const QHash<QString, QRgb> kColours = {
        {QStringLiteral("red"), qRgb(255, 0, 0)},       {QStringLiteral("green"), qRgb(0, 255, 0)},
        {QStringLiteral("blue"), qRgb(0, 0, 255)},      {QStringLiteral("cyan"), qRgb(0, 255, 255)},
        {QStringLiteral("magenta"), qRgb(255, 0, 255)}, {QStringLiteral("yellow"), qRgb(255, 255, 0)},
    };
    return kColours.value(lut.trimmed().toLower(), qRgb(255, 255, 255));
}

// A Windows FILETIME (100 ns since 1601) as LAS X writes it.
QDateTime fromFileTime(quint64 ft)
{
    if (ft == 0)
        return {};
    const qint64 ms = qint64(ft / 10000) - 11644473600000LL;
    if (ms < 0 || ms > 7258118400000LL) // before 1970 or after 2200: not a time stamp
        return {};
    return QDateTime::fromMSecsSinceEpoch(ms); // local time
}

// "N PLAN    10x/0.25 DRY " -> "N PLAN 10x/0.25 DRY"
QString tidy(const QString &s)
{
    return s.simplified();
}

// Length is the distance from the first pixel's centre to the last one's, in
// metres: n pixels span n - 1 steps. (Checked on this microscope: 3840 pixels
// over 1124.83 um at 10x is 0.29300 um, the sensor's 5.86 um / 20; dividing by
// n would give 0.29292.)
double umPerPixelOf(int n, double length, const QString &unit)
{
    if (n < 2 || length <= 0 || !(unit.isEmpty() || unit == QLatin1String("m")))
        return 0;
    return length * 1e6 / (n - 1);
}

struct XmlEntry {
    LifEntry e;
    bool isImage = false;
    struct Dim {
        int id;
        int size;
        qint64 inc;
        double origin, length;
        QString unit;
    };
    QList<Dim> dims;
    struct Chan {
        int resolution, dataType;
        qint64 inc;
        QString lut;
    };
    QList<Chan> chans;
    bool settingsSeen = false;
    int detectorLists = 0;
    QStringList detectors;     // active detectors of the first setting, in order
    QStringList wideField;     // active wide-field channels: cube or name
    bool inTimeStamps = false;
};

void addInfo(LifEntry &e, const QString &key, const QString &value)
{
    if (!value.trimmed().isEmpty())
        e.info.append({key, value.trimmed()});
}

QString fmt(double v, int precision = 4)
{
    return QString::number(v, 'g', precision);
}

// The settings an image was recorded with, from the first camera or confocal
// setting in its HardwareSetting attachment.
void readSettings(XmlEntry &x, const QXmlStreamAttributes &a, bool confocal)
{
    LifEntry &e = x.e;
    const auto attr = [&](const char *name) { return a.value(QLatin1String(name)).toString(); };
    e.microscope = tidy(attr("MicroscopeModel"));
    e.objective = tidy(attr("ObjectiveName"));
    e.magnification = number(a.value(QLatin1String("Magnification")));
    e.numericalAperture = number(a.value(QLatin1String("NumericalAperture")));
    addInfo(e, QObject::tr("Microscope"), e.microscope);
    addInfo(e, QObject::tr("Objective"), e.objective);
    if (e.magnification > 0)
        addInfo(e, QObject::tr("Magnification"), QStringLiteral("%1×").arg(fmt(e.magnification)));
    if (e.numericalAperture > 0)
        addInfo(e, QObject::tr("Numerical aperture"), fmt(e.numericalAperture, 3));
    addInfo(e, QObject::tr("Immersion"), attr("Immersion"));
    if (confocal) {
        if (const double z = number(a.value(QLatin1String("Zoom"))); z > 0)
            addInfo(e, QObject::tr("Zoom"), fmt(z));
        const double pinhole = number(a.value(QLatin1String("Pinhole")));
        const double airy = number(a.value(QLatin1String("PinholeAiry")));
        if (pinhole > 0)
            addInfo(e, QObject::tr("Pinhole"),
                    airy > 0 ? QObject::tr("%1 µm (%2 AU)").arg(fmt(pinhole * 1e6), fmt(airy, 3))
                             : QObject::tr("%1 µm").arg(fmt(pinhole * 1e6)));
        if (const double s = number(a.value(QLatin1String("ScanSpeed"))); s > 0)
            addInfo(e, QObject::tr("Scan speed"), QObject::tr("%1 Hz").arg(fmt(s)));
        if (const double d = number(a.value(QLatin1String("PixelDwellTime"))); d > 0)
            addInfo(e, QObject::tr("Pixel dwell time"), QObject::tr("%1 µs").arg(fmt(d * 1e6)));
        const int fa = attr("FrameAverage").toInt(), la = attr("LineAverage").toInt();
        if (fa > 1 || la > 1)
            addInfo(e, QObject::tr("Averaging"), QObject::tr("frame %1, line %2").arg(std::max(fa, 1)).arg(std::max(la, 1)));
        addInfo(e, QObject::tr("Scan mode"), attr("ScanMode"));
    } else {
        addInfo(e, QObject::tr("Image format"), attr("BinningText"));
        if (const double g = number(a.value(QLatin1String("GainValue"))); g > 0)
            addInfo(e, QObject::tr("Gain"), fmt(g));
    }
}

// Walks Element/Children/Element, building the tree and collecting the images.
void parseElement(QXmlStreamReader &r, const QString &xml, const QString &parentPath, LifNode &node,
                  QList<XmlEntry> &out)
{
    XmlEntry x;
    LifEntry &e = x.e;
    e.name = r.attributes().value(QStringLiteral("Name")).toString();
    e.uniqueId = r.attributes().value(QStringLiteral("UniqueID")).toString();
    e.path = parentPath;
    node.name = e.name;
    const qint64 after = r.characterOffset();
    e.xmlStart = std::max<qint64>(0, xml.lastIndexOf(QLatin1String("<Element"), int(std::max<qint64>(0, after - 1))));
    const QString here = parentPath.isEmpty() ? e.name : parentPath + QLatin1Char('/') + e.name;

    while (!r.atEnd()) {
        r.readNext();
        if (r.isEndElement()) {
            if (r.name() == QLatin1String("Element"))
                break;
            if (r.name() == QLatin1String("TimeStampList"))
                x.inTimeStamps = false;
            continue;
        }
        if (r.isCharacters()) {
            // newer files: the time stamps as hexadecimal FILETIMEs
            if (x.inTimeStamps && !e.acquired.isValid()) {
                const QString first = r.text().toString().trimmed().section(QLatin1Char(' '), 0, 0);
                bool ok = false;
                const quint64 ft = first.toULongLong(&ok, 16);
                if (ok)
                    e.acquired = fromFileTime(ft);
            }
            continue;
        }
        if (!r.isStartElement())
            continue;
        const QStringView tag = r.name();
        const QXmlStreamAttributes a = r.attributes();
        if (tag == QLatin1String("Element")) {
            node.children.append(LifNode());
            parseElement(r, xml, here, node.children.last(), out); // a folder inside a folder
        } else if (tag == QLatin1String("Image")) {
            x.isImage = true;
        } else if (tag == QLatin1String("Memory")) {
            e.blockId = a.value(QStringLiteral("MemoryBlockID")).toString();
        } else if (tag == QLatin1String("ChannelDescription")) {
            x.chans.append({a.value(QStringLiteral("Resolution")).toInt(), a.value(QStringLiteral("DataType")).toInt(),
                            a.value(QStringLiteral("BytesInc")).toLongLong(),
                            a.value(QStringLiteral("LUTName")).toString()});
        } else if (tag == QLatin1String("DimensionDescription")) {
            const QString id = a.value(QStringLiteral("DimID")).toString();
            // DimID is "X"/"Y"/"Z"/"T" in the sidecar XML and a number inside the file
            int n = id.toInt();
            if (id == QLatin1String("X"))
                n = LifDimX;
            else if (id == QLatin1String("Y"))
                n = LifDimY;
            else if (id == QLatin1String("Z"))
                n = LifDimZ;
            else if (id == QLatin1String("T"))
                n = LifDimT;
            x.dims.append({n, a.value(QStringLiteral("NumberOfElements")).toInt(),
                           a.value(QStringLiteral("BytesInc")).toLongLong(), number(a.value(QStringLiteral("Origin"))),
                           number(a.value(QStringLiteral("Length"))), a.value(QStringLiteral("Unit")).toString()});
        } else if (tag == QLatin1String("Attachment")) {
            const QStringView kind = a.value(QStringLiteral("Name"));
            if (kind == QLatin1String("TileScanInfo")) {
                e.tileFlipX = a.value(QStringLiteral("FlipX")).toInt() != 0;
                e.tileFlipY = a.value(QStringLiteral("FlipY")).toInt() != 0;
                e.tileSwapXY = a.value(QStringLiteral("SwapXY")).toInt() != 0;
            } else if (kind == QLatin1String("HardwareSetting") && e.software.isEmpty()) {
                e.software = a.value(QStringLiteral("Software")).toString();
                const QString system = a.value(QStringLiteral("SystemTypeName")).toString();
                const QString source = a.value(QStringLiteral("DataSourceTypeName")).toString();
                addInfo(e, QObject::tr("System"),
                        source.isEmpty() ? system : system.isEmpty() ? source : system + QStringLiteral(" (") + source + QLatin1Char(')'));
            }
        } else if (tag == QLatin1String("Tile")) {
            LifTile t;
            t.fieldX = a.value(QStringLiteral("FieldX")).toInt();
            t.fieldY = a.value(QStringLiteral("FieldY")).toInt();
            t.hasPosition = a.hasAttribute(QStringLiteral("PosX")) && a.hasAttribute(QStringLiteral("PosY"));
            t.posX = number(a.value(QStringLiteral("PosX")));
            t.posY = number(a.value(QStringLiteral("PosY")));
            e.tiles.append(t);
        } else if ((tag == QLatin1String("ATLCameraSettingDefinition") || tag == QLatin1String("ATLConfocalSettingDefinition"))
                   && !x.settingsSeen) {
            x.settingsSeen = true;
            readSettings(x, a, tag == QLatin1String("ATLConfocalSettingDefinition"));
        } else if (tag == QLatin1String("WideFieldChannelConfigurator") && e.camera.isEmpty()) {
            e.camera = a.value(QStringLiteral("CameraName")).toString();
            if (e.camera.isEmpty())
                e.camera = a.value(QStringLiteral("CameraNamePure")).toString();
        } else if (tag == QLatin1String("WideFieldChannelInfo")) {
            if (a.value(QStringLiteral("Active")).toInt() != 0 || !a.hasAttribute(QStringLiteral("Active"))) {
                if (e.exposureMs <= 0)
                    e.exposureMs = number(a.value(QStringLiteral("ExposureTime"))) * 1000.0;
                QString n = a.value(QStringLiteral("UserDefName")).toString().trimmed();
                if (n.isEmpty())
                    n = a.value(QStringLiteral("FluoCubeName")).toString().trimmed();
                if (n.isEmpty())
                    n = a.value(QStringLiteral("ContrastingMethodName")).toString().trimmed();
                x.wideField << n;
            }
        } else if (tag == QLatin1String("DetectorList")) {
            ++x.detectorLists;
        } else if (tag == QLatin1String("Detector") && x.detectorLists == 1) {
            if (a.value(QStringLiteral("IsActive")).toInt() != 0) {
                QString n = a.value(QStringLiteral("Name")).toString().trimmed();
                const QString dye = a.value(QStringLiteral("DyeName")).toString().section(QLatin1Char('/'), -1).trimmed();
                if (!dye.isEmpty())
                    n += QStringLiteral(" (") + dye + QLatin1Char(')');
                x.detectors << n;
            }
        } else if (tag == QLatin1String("TimeStampList")) {
            x.inTimeStamps = true;
        } else if (tag == QLatin1String("TimeStamp") && !e.acquired.isValid()
                   && a.hasAttribute(QStringLiteral("HighInteger"))) {
            // older files: one element per time stamp
            const quint64 hi = a.value(QStringLiteral("HighInteger")).toULongLong();
            const quint64 lo = a.value(QStringLiteral("LowInteger")).toULongLong();
            if (x.isImage) // the file's own save time (on the top element) is not an acquisition
                e.acquired = fromFileTime(hi << 32 | lo);
        }
    }
    e.xmlLength = std::max<qint64>(0, r.characterOffset() - e.xmlStart);
    if (!x.isImage || e.blockId.isEmpty())
        return;

    // the channels' names: the detectors or wide-field channels, when the
    // file lists exactly one per channel
    const QStringList &names = x.detectors.size() == x.chans.size() ? x.detectors
                               : x.wideField.size() == x.chans.size() ? x.wideField
                                                                        : QStringList();
    for (int i = 0; i < x.chans.size(); ++i) {
        const auto &c = x.chans[i];
        LifChannel ch;
        ch.lut = c.lut;
        ch.colour = lutColour(c.lut);
        ch.isFloat = c.dataType == 1;
        ch.bits = c.resolution > 0 ? c.resolution : 8;
        ch.sampleBytes = ch.isFloat || ch.bits > 16 ? 4 : ch.bits > 8 ? 2 : 1;
        ch.bytesInc = c.inc;
        ch.name = names.value(i).trimmed();
        if (ch.name.isEmpty())
            ch.name = c.lut.isEmpty() ? QObject::tr("Channel %1").arg(i + 1) : c.lut;
        e.channels.append(ch);
    }
    for (const auto &d : x.dims) {
        if (d.id == LifDimX) {
            e.width = d.size;
            e.xInc = d.inc;
            e.umPerPixel = umPerPixelOf(d.size, d.length, d.unit);
        } else if (d.id == LifDimY) {
            e.height = d.size;
            e.yInc = d.inc;
            e.umPerPixelY = umPerPixelOf(d.size, d.length, d.unit);
        } else {
            LifDimension pd;
            pd.id = d.id;
            pd.size = d.size;
            pd.bytesInc = d.inc;
            pd.origin = d.origin;
            pd.length = d.length;
            pd.unit = d.unit;
            e.planeDims.append(pd);
        }
    }
    if (e.umPerPixelY <= 0)
        e.umPerPixelY = e.umPerPixel;
    if (e.exposureMs > 0)
        addInfo(e, QObject::tr("Exposure"), QObject::tr("%1 ms").arg(fmt(e.exposureMs)));
    addInfo(e, QObject::tr("Camera"), e.camera);
    if (e.acquired.isValid())
        e.info.prepend({QObject::tr("Acquired"), QLocale().toString(e.acquired, QLocale::LongFormat)});
    addInfo(e, QObject::tr("Software"), e.software);
    node.image = int(out.size());
    out.append(x);
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
    qint64 offset = 0;
    qint64 size = 0;
};

// Walks the memory blocks after the header. `wide` selects the 64-bit size
// field of version 2; version 1 files use 32 bits.
bool readBlocks(QFile &f, bool wide, QHash<QString, Block> &out)
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
        if (!readU8(f, m2) || m2 != kMarker || !readU32(f, chars) || chars > 4096 || !readUtf16(f, chars, id))
            return false;
        Block b;
        b.offset = f.pos();
        // The data must be in the file: a damaged size field must not send a
        // reader (or an allocation) past its end. A file cut short keeps the
        // images before the cut.
        if (memSize > quint64(f.size() - b.offset))
            return true;
        b.size = qint64(memSize);
        out.insert(id, b);
        if (!f.seek(f.pos() + b.size))
            return false;
    }
    return true;
}

// acc += n * inc, false when it would pass `limit` (no overflow on the way)
bool addProduct(qint64 &acc, qint64 n, qint64 inc, qint64 limit)
{
    if (n < 0 || inc < 0)
        return false;
    if (n == 0 || inc == 0)
        return true;
    if (n > (limit - acc) / inc)
        return false;
    acc += n * inc;
    return acc <= limit;
}

// Why an entry cannot be read safely, or empty when it can. Every sample of
// every channel and plane must lie inside the data block.
QString layoutProblem(const LifEntry &e)
{
    if (e.width <= 0 || e.height <= 0)
        return QObject::tr("The image has no size.");
    if (e.width > kMaxSide || e.height > kMaxSide)
        return QObject::tr("The image size (%1 x %2) is not plausible.").arg(e.width).arg(e.height);
    if (e.channels.isEmpty() || e.channels.size() > kMaxChannels)
        return QObject::tr("The image has %1 channels.").arg(e.channels.size());
    if (e.planeDims.size() > kMaxDims)
        return QObject::tr("The image has %1 dimensions.").arg(e.planeDims.size() + 2);
    int widest = 1;
    for (const LifChannel &c : e.channels) {
        if (c.bytesInc < 0)
            return QObject::tr("The channel layout is damaged (a negative offset).");
        if (c.isFloat && c.sampleBytes != 4)
            return QObject::tr("%1-bit floating point samples are not supported.").arg(c.bits);
        widest = std::max(widest, c.sampleBytes);
    }
    if (e.xInc < widest)
        return QObject::tr("The pixel layout is damaged (a pixel is smaller than its sample).");
    if (e.yInc < qint64(e.width) * e.xInc && e.height > 1)
        return QObject::tr("The row layout is damaged (a row is shorter than its pixels).");
    // the last sample: every dimension at its last index
    qint64 extent = widest;
    const qint64 limit = e.dataBytes;
    bool fits = addProduct(extent, e.width - 1, e.xInc, limit) && addProduct(extent, e.height - 1, e.yInc, limit);
    for (const LifDimension &d : e.planeDims) {
        if (d.size < 1 || d.bytesInc < 0)
            return QObject::tr("The %1 dimension is damaged.").arg(d.label());
        fits = fits && addProduct(extent, d.size - 1, d.bytesInc, limit);
    }
    if (!fits)
        return QObject::tr("The image data is shorter than its size says (%1 bytes).").arg(e.dataBytes);
    for (const LifChannel &c : e.channels)
        if (c.bytesInc > e.dataBytes - extent)
            return QObject::tr("The channel layout is damaged (a channel lies outside the image data).");
    return QString();
}

QString openError(const QString &path, const QFile &f)
{
    return QObject::tr("Cannot open %1: %2").arg(QFileInfo(path).fileName(), f.errorString());
}

// Reads one channel of the plane that starts `planeOffset` bytes into the
// block, every `s`-th pixel and row.
bool readRawPlane(QFile &f, const LifEntry &e, const LifChannel &c, qint64 planeOffset, int s, LifPlane &out,
                  QString *error, const std::atomic<bool> *cancel)
{
    const int ow = (e.width + s - 1) / s, oh = (e.height + s - 1) / s;
    try {
        out.px.assign(size_t(ow) * oh, 0);
    } catch (const std::bad_alloc &) {
        if (error)
            *error = QObject::tr("Not enough memory for a %1 x %2 image.").arg(ow).arg(oh);
        return false;
    }
    out.width = ow;
    out.height = oh;
    out.bits = c.isFloat || c.sampleBytes == 4 ? 16 : std::min(16, c.bits);
    const qint64 base = e.dataOffset + c.bytesInc + planeOffset;
    const qint64 span = qint64(e.width - 1) * e.xInc + c.sampleBytes; // one row of this channel
    const qint64 step = qint64(s) * e.xInc;
    std::vector<float> wide; // 32-bit samples, mapped to 16 bits at the end
    if (c.sampleBytes == 4)
        wide.resize(out.px.size());
    // consecutive rows in one read when every row is wanted
    const int rowsPerRead = s == 1 ? int(std::clamp<qint64>(kChunkBytes / std::max<qint64>(1, e.yInc), 1, oh)) : 1;
    QByteArray buf;
    for (int oy = 0; oy < oh; oy += rowsPerRead) {
        if (cancel && cancel->load())
            return false;
        const int rows = std::min(rowsPerRead, oh - oy);
        const qint64 start = base + qint64(oy) * s * e.yInc;
        const qint64 bytes = qint64(rows - 1) * e.yInc + span;
        if (!f.seek(start) || (buf = f.read(bytes)).size() != bytes) {
            if (error)
                *error = QObject::tr("The image data ends at row %1 of %2.").arg(oy * s).arg(e.height);
            return false;
        }
        for (int r = 0; r < rows; ++r) {
            const uchar *src = reinterpret_cast<const uchar *>(buf.constData()) + qint64(r) * e.yInc;
            const size_t row = size_t(oy + r) * ow;
            uint16_t *dst = out.px.data() + row;
            switch (c.sampleBytes) {
            case 1:
                for (int x = 0; x < ow; ++x)
                    dst[x] = src[x * step];
                break;
            case 2:
                for (int x = 0; x < ow; ++x) {
                    const uchar *p = src + x * step;
                    dst[x] = uint16_t(p[0] | p[1] << 8);
                }
                break;
            default:
                for (int x = 0; x < ow; ++x) {
                    const uchar *p = src + x * step;
                    const quint32 u = quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24;
                    float v;
                    if (c.isFloat)
                        std::memcpy(&v, &u, 4);
                    else
                        v = float(u);
                    wide[row + x] = std::isfinite(v) ? v : 0.f;
                }
            }
        }
    }
    if (!wide.empty()) {
        // 32-bit samples are mapped onto 16 bits over the range this plane uses
        const auto [lo, hi] = std::minmax_element(wide.begin(), wide.end());
        const float a = *lo, range = *hi - *lo;
        const float scale = range > 0 ? 65535.f / range : 0.f;
        for (size_t i = 0; i < wide.size(); ++i)
            out.px[i] = saturate16((wide[i] - a) * scale);
    }
    return true;
}

qint64 planeOffset(const LifEntry &e, const LifCoord &coord)
{
    qint64 off = 0;
    for (int i = 0; i < e.planeDims.size(); ++i)
        off += qint64(std::clamp(coord.value(i), 0, e.planeDims[i].size - 1)) * e.planeDims[i].bytesInc;
    return off;
}

// One channel at `coord`, the tiles put together when asked.
bool readPlaneOrMosaic(QFile &f, const LifEntry &e, int channel, const LifCoord &coord, bool merge, int s,
                       LifPlane &out, QString *error, const std::atomic<bool> *cancel)
{
    const LifChannel &c = e.channels[channel];
    const int m = e.dimIndex(LifDimMosaic);
    if (!merge || m < 0 || e.planeDims[m].size < 2)
        return readRawPlane(f, e, c, planeOffset(e, coord), s, out, error, cancel);

    QList<QPoint> origins;
    const QSize size = lifMosaicSize(e, &origins);
    const int ow = (size.width() + s - 1) / s, oh = (size.height() + s - 1) / s;
    if (qint64(ow) > kMaxSide || qint64(oh) > kMaxSide) {
        if (error)
            *error = QObject::tr("The merged tile scan would be %1 x %2 pixels.").arg(size.width()).arg(size.height());
        return false;
    }
    try {
        out.px.assign(size_t(ow) * oh, 0);
    } catch (const std::bad_alloc &) {
        if (error)
            *error = QObject::tr("Not enough memory for a %1 x %2 image.").arg(ow).arg(oh);
        return false;
    }
    out.width = ow;
    out.height = oh;
    LifCoord at = coord;
    while (at.size() < e.planeDims.size())
        at.append(0);
    LifPlane tile;
    for (int t = 0; t < e.planeDims[m].size; ++t) {
        at[m] = t;
        if (!readRawPlane(f, e, c, planeOffset(e, at), s, tile, error, cancel))
            return false;
        out.bits = tile.bits;
        // each tile where the stage was; a later tile covers the overlap
        const int x0 = origins.value(t).x() / s, y0 = origins.value(t).y() / s;
        for (int y = 0; y < tile.height; ++y) {
            const int oy = y0 + y;
            if (oy < 0 || oy >= oh)
                continue;
            for (int x = 0; x < tile.width; ++x) {
                const int ox = x0 + x;
                if (ox >= 0 && ox < ow)
                    out.px[size_t(oy) * ow + ox] = tile.px[size_t(y) * tile.width + x];
            }
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------- the model

QString LifDimension::label() const
{
    switch (id) {
    case LifDimZ: return QObject::tr("Z");
    case LifDimT: return QObject::tr("T");
    case LifDimLambda: return QObject::tr("λ");
    case LifDimRotation: return QObject::tr("Angle");
    case LifDimXT: return QObject::tr("XT");
    case LifDimTSlice: return QObject::tr("T slice");
    case LifDimLambdaEx: return QObject::tr("λ ex");
    case LifDimMosaic: return QObject::tr("Tile");
    default: return QObject::tr("Dim %1").arg(id);
    }
}

double LifDimension::step() const
{
    if (size < 2 || length == 0)
        return 0;
    const double s = std::abs(length) / (size - 1);
    // space is stored in metres
    return id == LifDimZ && (unit.isEmpty() || unit == QLatin1String("m")) ? s * 1e6 : s;
}

int LifEntry::dimIndex(int id) const
{
    for (int i = 0; i < planeDims.size(); ++i)
        if (planeDims[i].id == id)
            return i;
    return -1;
}

int LifEntry::sizeOf(int id) const
{
    const int i = dimIndex(id);
    return i < 0 ? 1 : planeDims[i].size;
}

qint64 LifEntry::planeCount() const
{
    qint64 n = 1;
    for (const LifDimension &d : planeDims)
        n *= std::max(1, d.size);
    return n;
}

QString LifEntry::summary() const
{
    QString s = QStringLiteral("%1 × %2").arg(width).arg(height);
    if (!colour || channels.size() != 3)
        s += channels.size() == 1 ? QObject::tr(" · 1 channel") : QObject::tr(" · %1 channels").arg(channels.size());
    for (const LifDimension &d : planeDims)
        if (d.size > 1)
            s += d.id == LifDimMosaic ? QObject::tr(" · %1 tiles").arg(d.size)
                                      : QStringLiteral(" · %1 %2").arg(d.size).arg(d.label().toLower());
    if (bitsPerSample != 8)
        s += QObject::tr(" · %1-bit").arg(channels.isEmpty() ? bitsPerSample : channels.first().bits);
    return s;
}

QString LifFileIndex::elementXml(const LifEntry &e) const
{
    if (e.xmlStart < 0 || e.xmlLength <= 0 || e.xmlStart + e.xmlLength > xml.size())
        return QString();
    // re-indented, so the attributes can be read
    QString pretty;
    QXmlStreamReader r(QStringView(xml).mid(e.xmlStart, e.xmlLength));
    QXmlStreamWriter w(&pretty);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(2);
    while (!r.atEnd()) {
        r.readNext();
        if (r.isWhitespace())
            continue;
        if (!r.isStartDocument() && !r.isEndDocument())
            w.writeCurrentToken(r);
    }
    return r.hasError() ? QString(QStringView(xml).mid(e.xmlStart, e.xmlLength)) : pretty;
}

namespace {

// Leaves out folders that hold no image (LAS X's "FrameProperties" and such)
// and renumbers the images in tree order.
bool pruneTree(LifNode &n, const QList<XmlEntry> &all, const QHash<int, int> &keep, QList<LifEntry> &images)
{
    if (n.image >= 0) {
        const auto it = keep.find(n.image);
        if (it == keep.end()) {
            n.image = -1;
        } else {
            images.append(all[n.image].e);
            n.image = int(images.size() - 1);
        }
    }
    QList<LifNode> kept;
    for (LifNode &c : n.children)
        if (pruneTree(c, all, keep, images))
            kept.append(c);
    n.children = kept;
    return n.image >= 0 || !n.children.isEmpty();
}

} // namespace

bool readLif(const QString &path, LifFileIndex &out, QString *error)
{
    out = LifFileIndex();
    out.path = path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = openError(path, f);
        return false;
    }
    if (!readHeader(f, out.xml, error))
        return false;

    QList<XmlEntry> entries;
    {
        QXmlStreamReader r(out.xml);
        bool rootSeen = false;
        while (!r.atEnd()) {
            r.readNext();
            if (!r.isStartElement())
                continue;
            if (r.name() == QLatin1String("LMSDataContainerHeader")) {
                const int v = r.attributes().value(QStringLiteral("Version")).toInt();
                if (v > 0)
                    out.version = v;
            } else if (r.name() == QLatin1String("Element") && !rootSeen) {
                rootSeen = true;
                parseElement(r, out.xml, QString(), out.root, entries);
            }
        }
        if (r.hasError()) {
            if (error)
                *error = QObject::tr("The .lif index is not valid XML: %1").arg(r.errorString());
            return false;
        }
    }
    if (out.root.name.isEmpty())
        out.root.name = QFileInfo(path).fileName();

    QHash<QString, Block> blocks;
    if (!readBlocks(f, out.version >= 2, blocks)) {
        if (error)
            *error = QObject::tr("The .lif image data is damaged.");
        return false;
    }

    QHash<int, int> keep;
    for (int i = 0; i < entries.size(); ++i) {
        LifEntry &le = entries[i].e;
        const auto it = blocks.constFind(le.blockId);
        if (it == blocks.constEnd() || it->size <= 0)
            continue; // the XML names a block the file does not contain (or an empty one)
        le.dataOffset = it->offset;
        le.dataBytes = it->size;
        int deepest = 1, red = -1, green = -1, blue = -1;
        for (int c = 0; c < le.channels.size(); ++c) {
            deepest = std::max(deepest, le.channels[c].sampleBytes);
            const QString lut = le.channels[c].lut.toLower();
            if (lut == QLatin1String("red"))
                red = c;
            else if (lut == QLatin1String("green"))
                green = c;
            else if (lut == QLatin1String("blue"))
                blue = c;
        }
        le.bitsPerSample = deepest * 8;
        // Older files may list three channels without saying which is which:
        // interleaved, they are Leica's BGR.
        if (le.channels.size() == 3 && red < 0 && green < 0 && blue < 0 && le.xInc >= 3 * le.channels[0].sampleBytes
            && le.channels[0].bytesInc < le.xInc && le.channels[2].bytesInc < le.xInc) {
            const QString luts[3] = {QStringLiteral("Blue"), QStringLiteral("Green"), QStringLiteral("Red")};
            for (int c = 0; c < 3; ++c) {
                le.channels[c].lut = luts[c];
                le.channels[c].colour = lutColour(luts[c]);
                le.channels[c].name = luts[c];
            }
            red = 2;
            green = 1;
            blue = 0;
        }
        le.colour = le.channels.size() == 3 && red >= 0 && green >= 0 && blue >= 0;
        // a damaged entry is left out; the rest of the file stays readable
        if (const QString p = layoutProblem(le); !p.isEmpty()) {
            out.problems << QStringLiteral("\"%1\": %2").arg(le.name, p);
            continue;
        }
        keep.insert(i, i);
    }
    pruneTree(out.root, entries, keep, out.images);
    if (out.images.isEmpty() && error) {
        *error = QObject::tr("%1 holds no images this program can read.").arg(QFileInfo(path).fileName());
        if (!out.problems.isEmpty())
            *error += QLatin1Char(' ') + out.problems.first();
    }
    return !out.images.isEmpty();
}

bool readLifIndex(const QString &path, QList<LifEntry> &out, QString *error)
{
    LifFileIndex index;
    const bool ok = readLif(path, index, error);
    out = index.images;
    return ok;
}

QSize lifMosaicSize(const LifEntry &e, QList<QPoint> *tileOrigins)
{
    const int n = e.sizeOf(LifDimMosaic);
    QList<QPoint> origins;
    bool byStage = e.tiles.size() >= n && e.umPerPixel > 0;
    for (int i = 0; byStage && i < n; ++i)
        byStage = e.tiles[i].hasPosition;
    if (byStage) {
        // the stage positions, in pixels; the image axes may be flipped or
        // swapped against the stage
        QList<QPointF> p;
        const double mPerPx = e.umPerPixel * 1e-6, mPerPxY = (e.umPerPixelY > 0 ? e.umPerPixelY : e.umPerPixel) * 1e-6;
        for (int i = 0; i < n; ++i) {
            double x = e.tiles[i].posX, y = e.tiles[i].posY;
            if (e.tileSwapXY)
                std::swap(x, y);
            if (e.tileFlipX)
                x = -x;
            if (e.tileFlipY)
                y = -y;
            p.append(QPointF(x / mPerPx, y / mPerPxY));
        }
        double minX = p[0].x(), minY = p[0].y();
        for (const QPointF &q : p) {
            minX = std::min(minX, q.x());
            minY = std::min(minY, q.y());
        }
        for (const QPointF &q : p)
            origins.append(QPoint(int(std::lround(q.x() - minX)), int(std::lround(q.y() - minY))));
        // positions that put every tile in one place are not positions
        bool distinct = false;
        for (const QPoint &o : origins)
            distinct = distinct || o != origins[0];
        if (!distinct && n > 1)
            origins.clear();
    }
    if (origins.isEmpty()) {
        // the grid LAS X numbers the fields in
        for (int i = 0; i < n; ++i) {
            const LifTile t = e.tiles.value(i);
            const int fx = i < e.tiles.size() ? t.fieldX : i, fy = i < e.tiles.size() ? t.fieldY : 0;
            origins.append(QPoint(fx * e.width, fy * e.height));
        }
    }
    int w = 0, h = 0;
    for (const QPoint &o : origins) {
        w = std::max(w, o.x() + e.width);
        h = std::max(h, o.y() + e.height);
    }
    if (tileOrigins)
        *tileOrigins = origins;
    return QSize(w, h);
}

bool readLifPlane(const QString &path, const LifEntry &e, int channel, const LifRequest &req, LifPlane &out,
                  QString *error, const std::atomic<bool> *cancel)
{
    out = LifPlane();
    // the index checked this already; an entry made or changed elsewhere is checked again
    if (const QString p = layoutProblem(e); !p.isEmpty()) {
        if (error)
            *error = p;
        return false;
    }
    if (channel < 0 || channel >= e.channels.size()) {
        if (error)
            *error = QObject::tr("The image has no channel %1.").arg(channel + 1);
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = openError(path, f);
        return false;
    }
    if (e.dataOffset < 0 || e.dataOffset + e.dataBytes > f.size()) {
        if (error)
            *error = QObject::tr("Cannot reach the image data.");
        return false;
    }
    const int s = std::max(1, req.subsample);
    const int pd = req.projectDim;
    if (pd < 0 || pd >= e.planeDims.size() || e.planeDims[pd].size < 2)
        return readPlaneOrMosaic(f, e, channel, req.coord, req.mergeTiles, s, out, error, cancel);

    // the brightest value along the dimension (a maximum intensity projection)
    LifCoord at = req.coord;
    while (at.size() < e.planeDims.size())
        at.append(0);
    LifPlane one;
    for (int k = 0; k < e.planeDims[pd].size; ++k) {
        at[pd] = k;
        if (!readPlaneOrMosaic(f, e, channel, at, req.mergeTiles, s, k == 0 ? out : one, error, cancel))
            return false;
        if (k > 0)
            for (size_t i = 0; i < out.px.size(); ++i)
                out.px[i] = std::max(out.px[i], one.px[i]);
    }
    return true;
}

bool readLifChannels(const QString &path, const LifEntry &e, const LifRequest &req, QList<LifPlane> &out,
                     QString *error, const std::atomic<bool> *cancel)
{
    out.clear();
    for (int c = 0; c < e.channels.size(); ++c) {
        LifPlane p;
        if (!readLifPlane(path, e, c, req, p, error, cancel)) {
            out.clear();
            return false;
        }
        out.append(std::move(p));
    }
    return true;
}

QList<LifChannelDisplay> defaultLifDisplay(const LifEntry &e)
{
    QList<LifChannelDisplay> d;
    for (const LifChannel &c : e.channels) {
        LifChannelDisplay cd;
        cd.colour = c.colour;
        cd.low = 0;
        cd.high = c.maxValue();
        d.append(cd);
    }
    return d;
}

void autoLifRange(const LifPlane &p, int &low, int &high, double fraction)
{
    low = 0;
    high = (1 << std::clamp(p.bits, 1, 16)) - 1;
    if (p.px.empty())
        return;
    std::vector<quint32> hist(65536, 0);
    for (uint16_t v : p.px)
        ++hist[v];
    const quint64 total = p.px.size();
    const quint64 cut = quint64(std::max(0.0, fraction) * double(total));
    quint64 sum = 0;
    int lo = 0, hi = 65535;
    for (; lo < 65535; ++lo) {
        sum += hist[size_t(lo)];
        if (sum > cut)
            break;
    }
    sum = 0;
    for (; hi > 0; --hi) {
        sum += hist[size_t(hi)];
        if (sum > cut)
            break;
    }
    if (hi <= lo) { // a flat image: show it as it is
        hi = std::max(lo + 1, std::min(65535, lo + 1));
        if (lo > 0)
            --lo;
    }
    low = lo;
    high = hi;
}

Image16 composeLif(const QList<LifPlane> &planes, const QList<LifChannelDisplay> &display)
{
    if (planes.isEmpty())
        return {};
    const int w = planes[0].width, h = planes[0].height;
    Image16 out(w, h);
    struct Use {
        const uint16_t *px;
        float low, scale;
        float r, g, b;
    };
    std::vector<Use> uses;
    for (int c = 0; c < planes.size() && c < display.size(); ++c) {
        const LifChannelDisplay &d = display[c];
        if (!d.visible || planes[c].width != w || planes[c].height != h || planes[c].px.empty())
            continue;
        const float range = float(std::max(1, d.high - d.low));
        // 65535 at full intensity of a fully lit LUT
        uses.push_back({planes[c].px.data(), float(d.low), 1.f / range, qRed(d.colour) * (65535.f / 255.f),
                        qGreen(d.colour) * (65535.f / 255.f), qBlue(d.colour) * (65535.f / 255.f)});
    }
    if (uses.empty())
        return out;
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uint16_t *dst = out.row(y);
            const size_t row = size_t(y) * w;
            for (int x = 0; x < w; ++x) {
                float r = 0, g = 0, b = 0;
                for (const Use &u : uses) {
                    const float v = std::clamp((float(u.px[row + x]) - u.low) * u.scale, 0.f, 1.f);
                    r += v * u.r;
                    g += v * u.g;
                    b += v * u.b;
                }
                dst[3 * x] = saturate16(r);
                dst[3 * x + 1] = saturate16(g);
                dst[3 * x + 2] = saturate16(b);
            }
        }
    });
    return out;
}

ImageMetadata lifMetadata(const LifEntry &e)
{
    ImageMetadata m;
    m.software = QStringLiteral("Leica LAS X (.lif)");
    m.softwareVersion = e.software;
    m.umPerPixel = e.umPerPixel;
    m.width = e.width;
    m.height = e.height;
    m.bitDepth = e.channels.isEmpty() ? 8 : std::min(16, e.channels.first().bits);
    m.sample = e.name; // the name LAS X gave the image
    m.acquired = e.acquired;
    m.objective = e.objective;
    m.magnification = e.magnification;
    m.numericalAperture = e.numericalAperture;
    m.exposureMs = e.exposureMs;
    m.camera = e.camera;
    m.captureMode = QStringLiteral("lif");
    if (!e.microscope.isEmpty())
        m.microscope = QStringLiteral("Leica ") + e.microscope;
    else
        m.microscope.clear(); // not recorded; not necessarily this microscope
    return m;
}

bool readLifImage(const QString &path, const LifEntry &e, LoadedImage &out, QString *error)
{
    QList<LifPlane> planes;
    if (!readLifChannels(path, e, LifRequest(), planes, error))
        return false;
    try {
        out.data = composeLif(planes, defaultLifDisplay(e));
    } catch (const std::bad_alloc &) {
        if (error)
            *error = QObject::tr("Not enough memory for a %1 x %2 image.").arg(e.width).arg(e.height);
        return false;
    }
    out.sourceBitDepth = e.channels.first().bits <= 8 ? 8 : 16;
    out.path = path;
    out.hasMeta = true;
    out.meta = lifMetadata(e);
    out.meta.bitDepth = out.sourceBitDepth;
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
                // Length is the distance from the first pixel to the last, in
                // metres (n - 1 steps); 0 when uncalibrated
                const double metres = im.umPerPixel > 0 ? im.umPerPixel * 1e-6 * std::max(1, n - 1) : 0.0;
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

    // Written to a temporary file that replaces the target only once complete, so
    // a failed write (a full disk) never destroys an existing file of that name.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
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
                    // B, G, R as LAS X stores them, rounded like the TIFF writer
                    dst[3 * x] = uchar((b + 128) / 257);
                    dst[3 * x + 1] = uchar((g + 128) / 257);
                    dst[3 * x + 2] = uchar((r + 128) / 257);
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
                f.cancelWriting();
                return false;
            }
        }
    }
    // commit() reports any earlier failed write (headers included) and only
    // then replaces the target
    if (!f.commit()) {
        if (error)
            *error = QObject::tr("Writing %1 failed: %2").arg(QFileInfo(path).fileName(), f.errorString());
        return false;
    }
    return true;
}

} // namespace lm
