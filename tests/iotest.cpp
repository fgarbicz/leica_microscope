// Image I/O tests: TIFF round trip (8/16 bit, compression, calibration and
// metadata), robustness against corrupted files (fuzzing), the calibration
// repair in every format, and .lif: reading every kind of image LAS X writes,
// writing, and exporting.
#include "io/ImageIO.h"
#include "io/CalibrationRepair.h"
#include "io/LifExport.h"
#include "io/LifFile.h"
#include "liftestdata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QPoint>
#include <QSize>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <random>

using namespace lm;

static int g_failed = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            ++g_failed;                                                           \
            std::printf("  FAILED line %d: %s\n", __LINE__, #c);                  \
        }                                                                         \
    } while (0)

// ---- Leica .lif ------------------------------------------------------------

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

// The pages of a grey TIFF as this program writes them (strips, optional
// Deflate), and the first page's ImageDescription.
static bool readTiffPages(const QString &path, QList<std::vector<uint16_t>> &pages, QString &desc, int &width,
                          int &height, int &bits)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray d = f.readAll();
    const auto *b = reinterpret_cast<const uchar *>(d.constData());
    const auto u16 = [&](qint64 o) { return quint32(b[o] | b[o + 1] << 8); };
    const auto u32 = [&](qint64 o) { return quint32(b[o]) | quint32(b[o + 1]) << 8 | quint32(b[o + 2]) << 16 | quint32(b[o + 3]) << 24; };
    if (d.size() < 8 || d[0] != 'I' || d[1] != 'I' || u16(2) != 42)
        return false;
    quint32 ifd = u32(4);
    pages.clear();
    while (ifd != 0 && ifd + 2 < quint32(d.size())) {
        const int n = int(u16(ifd));
        quint32 compression = 1, stripCount = 0, offsetsAt = 0, countsAt = 0;
        bool firstDesc = desc.isEmpty() && pages.isEmpty();
        for (int i = 0; i < n; ++i) {
            const qint64 e = ifd + 2 + 12 * i;
            const quint32 tag = u16(e), type = u16(e + 2), count = u32(e + 4);
            const quint32 value = type == 3 && count == 1 ? u16(e + 8) : u32(e + 8);
            switch (tag) {
            case 256: width = int(value); break;
            case 257: height = int(value); break;
            case 258: bits = int(value); break;
            case 259: compression = value; break;
            case 270:
                if (firstDesc)
                    desc = QString::fromUtf8(d.mid(count > 4 ? value : e + 8, count)).remove(QChar(0));
                break;
            case 273:
                stripCount = count;
                offsetsAt = count == 1 ? quint32(e + 8) : value;
                break;
            case 279: countsAt = count == 1 ? quint32(e + 8) : value; break;
            }
        }
        QByteArray raw;
        for (quint32 s = 0; s < stripCount; ++s) {
            const QByteArray strip = d.mid(u32(offsetsAt + 4 * s), u32(countsAt + 4 * s));
            if (compression == 8) {
                const int expected = width * height * (bits / 8); // an upper bound for the strip
                QByteArray withLen(4, 0);
                withLen[0] = char((expected >> 24) & 0xff);
                withLen[1] = char((expected >> 16) & 0xff);
                withLen[2] = char((expected >> 8) & 0xff);
                withLen[3] = char(expected & 0xff);
                raw += qUncompress(withLen + strip);
            } else {
                raw += strip;
            }
        }
        std::vector<uint16_t> px(size_t(width) * height);
        if (raw.size() != qsizetype(px.size()) * (bits / 8))
            return false;
        const auto *r = reinterpret_cast<const uchar *>(raw.constData());
        for (size_t i = 0; i < px.size(); ++i)
            px[i] = bits == 16 ? uint16_t(r[2 * i] | r[2 * i + 1] << 8) : r[i];
        pages.append(px);
        ifd = u32(ifd + 2 + 12 * n);
    }
    return !pages.isEmpty();
}

static void lifTests(const QTemporaryDir &dir)
{
    std::printf("lif: write, read back, and keep the pixels and the scale\n");
    {
        const QString lifPath = dir.filePath(QStringLiteral("session.lif"));
        Image16 a(37, 19), b(64, 40);
        for (int y = 0; y < a.height; ++y)
            for (int x = 0; x < a.width; ++x) {
                a.row(y)[3 * x] = uint16_t((x * 1543) & 0xFF00);       // 8-bit writing keeps
                a.row(y)[3 * x + 1] = uint16_t((y * 2311) & 0xFF00);   // the high byte only
                a.row(y)[3 * x + 2] = uint16_t(((x + y) * 907) & 0xFF00);
            }
        for (int y = 0; y < b.height; ++y)
            for (int x = 0; x < b.width; ++x)
                b.row(y)[3 * x] = b.row(y)[3 * x + 1] = b.row(y)[3 * x + 2] = uint16_t((x ^ y) << 8);

        QList<LifImageOut> outs;
        outs.push_back({QStringLiteral("first 10x"), a, 0.2929, true});
        outs.push_back({QStringLiteral("second 40x"), b, 0.0732, true});
        QString err;
        CHECK(writeLif(lifPath, outs, QStringLiteral("test experiment"), &err));
        // DMI_LIF_OUT keeps a copy, so another implementation can check it
        if (const QByteArray keep = qgetenv("DMI_LIF_OUT"); !keep.isEmpty()) {
            QFile::remove(QString::fromLocal8Bit(keep));
            CHECK(QFile::copy(lifPath, QString::fromLocal8Bit(keep)));
        }

        QList<LifEntry> index;
        CHECK(readLifIndex(lifPath, index, &err));
        CHECK(index.size() == 2);
        if (index.size() == 2) {
            CHECK(index[0].name == QLatin1String("first 10x"));
            CHECK(index[0].width == 37 && index[0].height == 19);
            CHECK(index[1].width == 64 && index[1].height == 40);
            CHECK(index[0].channels.size() == 3 && index[0].colour);
            CHECK(index[0].planeDims.isEmpty() && index[0].planeCount() == 1);
            // the pixel size survives exactly (Length spans n - 1 pixels both ways)
            CHECK(std::abs(index[0].umPerPixel - 0.2929) < 1e-6);
            CHECK(std::abs(index[1].umPerPixel - 0.0732) < 1e-6);

            LoadedImage li;
            CHECK(readLifImage(lifPath, index[0], li, &err));
            CHECK(li.data.width == 37 && li.data.height == 19);
            CHECK(std::abs(li.meta.umPerPixel - 0.2929) < 1e-6);
            CHECK(li.meta.sample == QLatin1String("first 10x"));
            // the pixels survive the round trip (8-bit: rounded as the TIFF writer does)
            int worst = 0;
            for (int y = 0; y < a.height; ++y)
                for (int x = 0; x < a.width * 3; ++x)
                    worst = std::max(worst, std::abs(int(li.data.row(y)[x]) - int((a.row(y)[x] + 128) / 257) * 257));
            CHECK(worst == 0);

            LoadedImage li2;
            CHECK(readLifImage(lifPath, index[1], li2, &err));
            CHECK(li2.data.width == 64 && li2.data.height == 40);
        }
    }

    std::printf("lif: 16-bit colour round trip, over an existing file\n");
    {
        const QString lifPath = dir.filePath(QStringLiteral("session.lif")); // replaces the one above
        Image16 c(41, 23);
        for (int y = 0; y < c.height; ++y)
            for (int x = 0; x < c.width; ++x) {
                c.row(y)[3 * x] = uint16_t(x * 1543 + y * 7);      // distinct values per channel,
                c.row(y)[3 * x + 1] = uint16_t(y * 2311 + x * 3);  // low bytes included
                c.row(y)[3 * x + 2] = uint16_t((x + y) * 907 + 1);
            }
        QList<LifImageOut> outs;
        outs.push_back({QStringLiteral("deep"), c, 0.1465, false});
        QString err;
        CHECK(writeLif(lifPath, outs, QString(), &err));
        QList<LifEntry> index;
        CHECK(readLifIndex(lifPath, index, &err));
        CHECK(index.size() == 1);
        if (index.size() == 1) {
            const LifEntry &e = index[0];
            CHECK(e.bitsPerSample == 16 && e.xInc == 6 && e.channels.size() == 3);
            // written as LAS X does: blue, green, red at byte 0, 2, 4
            CHECK(e.channels.size() == 3 && e.channels[0].lut == QLatin1String("Blue") && e.channels[0].bytesInc == 0
                  && e.channels[2].lut == QLatin1String("Red") && e.channels[2].bytesInc == 4);
            LoadedImage li;
            CHECK(readLifImage(lifPath, e, li, &err));
            CHECK(li.data.width == c.width && li.data.height == c.height && li.data.px == c.px);

            // offsets outside the pixel or a row wider than its stride are refused
            LifPlane p;
            LifEntry bad = e;
            bad.channels[2].bytesInc = 5; // + 2 bytes > 6
            CHECK(!readLifPlane(lifPath, bad, 2, LifRequest(), p, &err) && !err.isEmpty());
            bad = e;
            bad.yInc = bad.width * bad.xInc - 1;
            CHECK(!readLifPlane(lifPath, bad, 0, LifRequest(), p, &err));
            bad = e;
            bad.width = bad.height = 1 << 28; // claims far more than the file holds
            bad.yInc = qint64(bad.width) * bad.xInc;
            CHECK(!readLifPlane(lifPath, bad, 0, LifRequest(), p, &err));
            bad = e;
            CHECK(!readLifPlane(lifPath, bad, 3, LifRequest(), p, &err)); // no such channel
            LoadedImage viaLoad;
            CHECK(loadImage(lifPath, viaLoad, &err) && viaLoad.data.px == c.px);

            // The same block described as planar channels (how a fluorescence
            // image stores them): three planes of w*h samples a plane apart.
            QFile raw(lifPath);
            CHECK(raw.open(QIODevice::ReadOnly) && raw.seek(e.dataOffset));
            const QByteArray block = raw.read(e.dataBytes);
            raw.close();
            const qint64 plane = qint64(c.width) * c.height * 2;
            CHECK(block.size() == 3 * plane);
            auto rawSample = [&](qint64 byteOffset) {
                const auto *b = reinterpret_cast<const uchar *>(block.constData()) + byteOffset;
                return uint16_t(b[0] | b[1] << 8);
            };
            LifEntry planar = e;
            planar.xInc = 2;
            planar.yInc = qint64(c.width) * 2;
            for (int ch = 0; ch < 3; ++ch)
                planar.channels[ch].bytesInc = ch * plane;
            bool planesMatch = true;
            for (int ch = 0; ch < 3; ++ch) {
                CHECK(readLifPlane(lifPath, planar, ch, LifRequest(), p, &err));
                planesMatch = planesMatch && p.width == c.width && p.height == c.height;
                for (int y = 0; planesMatch && y < c.height; ++y)
                    for (int x = 0; x < c.width; ++x)
                        if (p.at(x, y) != rawSample(ch * plane + (qint64(y) * c.width + x) * 2))
                            planesMatch = false;
            }
            CHECK(planesMatch);
            // a plane that would run past the data block is refused
            planar.channels[2].bytesInc = 2 * plane + 2;
            CHECK(!readLifPlane(lifPath, planar, 2, LifRequest(), p, &err) && !err.isEmpty());
        }
    }

    // the file every test below reads: see liftestdata.h
    using namespace liftest;
    QByteArray stack, camera;
    const QString xml = syntheticXml(stack, camera);
    const QDateTime when = syntheticTime();
    const auto dim = [](int id, int n, qint64 inc, const QString &len, const QString &unit = QStringLiteral("m")) {
        return dimXml(id, n, inc, len, unit);
    };
    const QString synth = dir.filePath(QStringLiteral("synthetic.lif"));
    CHECK(writeFile(synth, syntheticLif(xml, stack, camera)));

    std::printf("lif: the tree, the dimensions and the settings of a multi-dimensional file\n");
    LifFileIndex idx;
    QString err;
    CHECK(readLif(synth, idx, &err));
    CHECK(idx.images.size() == 2);
    CHECK(idx.problems.size() == 1 && idx.problems.value(0).contains(QLatin1String("broken")));
    // the empty folder and the damaged image are left out of the tree
    CHECK(idx.root.name == QLatin1String("synthetic.lif") && idx.root.children.size() == 2);
    if (idx.root.children.size() == 2) {
        const LifNode &folder = idx.root.children[0];
        CHECK(folder.name == QLatin1String("Folder A") && folder.image < 0 && folder.children.size() == 1
              && folder.children.value(0).image == 0);
        CHECK(idx.root.children[1].image == 1);
    }
    if (idx.images.size() != 2)
        return;
    const LifEntry &e = idx.images[0];
    CHECK(e.name == QLatin1String("stack") && e.path == QLatin1String("synthetic.lif/Folder A"));
    CHECK(e.width == W && e.height == H && !e.colour);
    CHECK(e.channels.size() == C && e.channels[0].bits == 12 && e.channels[0].sampleBytes == 2
          && e.channels[0].maxValue() == 4095);
    CHECK(e.channels[0].colour == qRgb(255, 0, 0) && e.channels[1].colour == qRgb(0, 255, 0));
    // two active detectors for two channels: they name the channels
    CHECK(e.channels[0].name == QLatin1String("HyD 1 (DAPI)") && e.channels[1].name == QLatin1String("HyD 3"));
    CHECK(e.planeDims.size() == 3 && e.dimIndex(LifDimZ) == 0 && e.dimIndex(LifDimMosaic) == 1
          && e.dimIndex(LifDimT) == 2);
    CHECK(e.sizeOf(LifDimZ) == Z && e.sizeOf(LifDimT) == T && e.sizeOf(LifDimMosaic) == M
          && e.sizeOf(LifDimLambda) == 1 && e.planeCount() == Z * T * M && e.hasTiles());
    CHECK(std::abs(e.umPerPixel - 0.5) < 1e-9 && std::abs(e.umPerPixelY - 0.5) < 1e-9);
    CHECK(std::abs(e.planeDims[0].step() - 2.0) < 1e-9);  // 4 um over 2 steps
    CHECK(std::abs(e.planeDims[2].step() - 2.5) < 1e-9);  // seconds
    CHECK(e.objective == QLatin1String("HC PL APO 63x/1.40 OIL") && e.magnification == 63
          && std::abs(e.numericalAperture - 1.4) < 1e-9);
    bool pinhole = false;
    for (const auto &kv : e.info)
        pinhole = pinhole || (kv.first == QLatin1String("Pinhole") && kv.second.contains(QStringLiteral("100 µm")));
    CHECK(pinhole);
    CHECK(e.summary().contains(QLatin1String("3 z")) && e.summary().contains(QLatin1String("2 tiles")));
    CHECK(idx.elementXml(e).contains(QLatin1String("ATLConfocalSettingDefinition"))
          && !idx.elementXml(e).contains(QLatin1String("camera: 10x")));

    const LifEntry &cam = idx.images[1];
    CHECK(cam.colour && cam.planeDims.isEmpty());
    CHECK(cam.objective == QLatin1String("N PLAN 10x/0.25 DRY") && cam.magnification == 10);
    CHECK(std::abs(cam.exposureMs - 10) < 1e-9 && cam.camera == QLatin1String("DMC6200-1"));
    CHECK(cam.acquired == when);
    CHECK(std::abs(cam.umPerPixel - 1.0) < 1e-9); // 3 um over 3 steps
    {
        const ImageMetadata m = lifMetadata(cam);
        CHECK(m.objective == cam.objective && m.numericalAperture == 0.25 && m.exposureMs == 10
              && m.microscope == QLatin1String("Leica DM2000") && m.acquired == when);
        LoadedImage li;
        CHECK(readLifImage(synth, cam, li, &err));
        // BGR on disk, RGB in the program, 8-bit scaled by 257
        CHECK(li.data.width == 4 && li.data.row(2)[3 * 1] == (200 + 9) * 257
              && li.data.row(2)[3 * 1 + 1] == (100 + 9) * 257 && li.data.row(2)[3 * 1 + 2] == (10 + 9) * 257);
    }

    std::printf("lif: every plane of every channel, a subsampled preview, a z projection, merged tiles\n");
    {
        bool all = true;
        LifPlane p;
        for (int t = 0; t < T; ++t)
            for (int m = 0; m < M; ++m)
                for (int z = 0; z < Z; ++z)
                    for (int c = 0; c < C; ++c) {
                        LifRequest r;
                        r.coord = {z, m, t};
                        if (!readLifPlane(synth, e, c, r, p, &err) || p.width != W || p.height != H || p.bits != 12) {
                            all = false;
                            continue;
                        }
                        for (int y = 0; y < H; ++y)
                            for (int x = 0; x < W; ++x)
                                all = all && p.at(x, y) == value(c, z, t, m, x, y);
                    }
        CHECK(all);

        LifRequest r;
        r.coord = {1, 1, 1};
        r.subsample = 2;
        CHECK(readLifPlane(synth, e, 1, r, p, &err) && p.width == 7 && p.height == 4);
        CHECK(p.at(3, 2) == value(1, 1, 1, 1, 6, 4) && p.at(6, 3) == value(1, 1, 1, 1, 12, 6));

        r = LifRequest();
        r.coord = {0, 1, 1};
        r.projectDim = 0; // the brightest along z: the last slice here
        CHECK(readLifPlane(synth, e, 0, r, p, &err) && p.at(5, 5) == value(0, Z - 1, 1, 1, 5, 5));

        QList<QPoint> origins;
        CHECK(lifMosaicSize(e, &origins) == QSize(10 + W, 1 + H));
        CHECK(origins == (QList<QPoint>{QPoint(0, 0), QPoint(10, 1)}));
        r = LifRequest();
        r.coord = {2, 0, 1};
        r.mergeTiles = true;
        CHECK(readLifPlane(synth, e, 1, r, p, &err) && p.width == 10 + W && p.height == 1 + H);
        CHECK(p.at(2, 3) == value(1, 2, 1, 0, 2, 3));        // the first tile alone
        CHECK(p.at(10 + 4, 1 + 2) == value(1, 2, 1, 1, 4, 2)); // the second covers the overlap
        CHECK(p.at(0, H) == 0 && p.at(10 + W - 1, 0) == 0);  // nothing was recorded there
        r.subsample = 3;
        CHECK(readLifPlane(synth, e, 1, r, p, &err) && p.width == (10 + W + 2) / 3 && p.height == (1 + H + 2) / 3);
        // merged and projected at once
        r = LifRequest();
        r.coord = {0, 0, 0};
        r.mergeTiles = true;
        r.projectDim = 0;
        CHECK(readLifPlane(synth, e, 0, r, p, &err) && p.at(10 + 1, 1 + 1) == value(0, Z - 1, 0, 1, 1, 1));
    }

    std::printf("lif: channels shown in their colours, automatic contrast\n");
    {
        QList<LifPlane> planes;
        LifRequest r;
        CHECK(readLifChannels(synth, e, r, planes, &err) && planes.size() == 2);
        QList<LifChannelDisplay> d = defaultLifDisplay(e);
        CHECK(d.size() == 2 && d[0].high == 4095 && d[0].colour == qRgb(255, 0, 0));
        d[0].low = 0;
        d[0].high = 10;   // red saturates
        d[1].low = 1000;
        d[1].high = 1100; // green: (v - 1000) / 100
        Image16 img = composeLif(planes, d);
        CHECK(img.width == W && img.height == H);
        const uint16_t g = value(1, 0, 0, 0, 3, 2);
        CHECK(img.row(2)[3 * 3] == 65535 && img.row(2)[3 * 3 + 2] == 0
              && std::abs(int(img.row(2)[3 * 3 + 1]) - int(std::lround((g - 1000) / 100.0 * 65535))) <= 1);
        d[0].visible = false;
        img = composeLif(planes, d);
        CHECK(img.row(2)[3 * 3] == 0);
        int lo = 0, hi = 0;
        autoLifRange(planes[1], lo, hi, 0.0);
        CHECK(lo == value(1, 0, 0, 0, 0, 0) && hi == value(1, 0, 0, 0, W - 1, H - 1));
        const QList<LifChannelDisplay> a = autoLifDisplay(e, planes);
        CHECK(a.size() == 2 && a[1].low >= lo && a[1].high <= hi && a[1].low < a[1].high);
        // a colour camera image is shown as recorded
        CHECK(autoLifDisplay(cam, planes).value(0).high == 255);
    }

    std::printf("lif: export as ImageJ hyperstacks and as pictures\n");
    {
        CHECK(lifImageFileName(e) == QLatin1String("Folder A - stack"));
        CHECK(lifImageFileName(cam) == QLatin1String("camera_ 10x_0.25"));
        CHECK(safeFileName(QStringLiteral(" a<b>:c. ")) == QLatin1String("a_b__c"));

        LifExportOptions o;
        o.kind = LifExportOptions::OriginalData;
        o.mergeTiles = false;
        LifExportItem item;
        item.image = 0;
        const QString out = dir.filePath(QStringLiteral("export"));
        qint64 lastDone = 0, lastTotal = 0;
        LifExportResult res = exportLif(idx, {item}, out, o, [&](qint64 done, qint64 total, const QString &) {
            lastDone = done;
            lastTotal = total;
            return true;
        });
        CHECK(res.failed.isEmpty() && !res.cancelled);
        CHECK(res.folder == QDir(out).filePath(QStringLiteral("synthetic")));
        CHECK(lastDone == lastTotal && lastTotal == qint64(C) * Z * T * M);
        const QString tile2 = QDir(res.folder).filePath(QStringLiteral("Folder A - stack_tile2.tif"));
        CHECK(res.written.contains(tile2) && res.written.contains(QDir(res.folder).filePath(QStringLiteral("images.csv"))));
        QList<std::vector<uint16_t>> pages;
        QString desc;
        int w = 0, h = 0, bits = 0;
        CHECK(readTiffPages(tile2, pages, desc, w, h, bits));
        CHECK(pages.size() == C * Z * T && w == W && h == H && bits == 16);
        CHECK(desc.startsWith(QLatin1String("ImageJ=")) && desc.contains(QLatin1String("channels=2\n"))
              && desc.contains(QLatin1String("slices=3\n")) && desc.contains(QLatin1String("frames=2\n"))
              && desc.contains(QLatin1String("hyperstack=true")) && desc.contains(QLatin1String("unit=micron"))
              && desc.contains(QLatin1String("spacing=2\n")) && desc.contains(QLatin1String("finterval=2.5\n")));
        // ImageJ's order: channel fastest, then slice, then frame
        bool order = pages.size() == C * Z * T;
        for (int t = 0; order && t < T; ++t)
            for (int z = 0; z < Z; ++z)
                for (int c = 0; c < C; ++c)
                    order = order && pages[c + C * (z + Z * t)][size_t(4 * W + 6)] == value(c, z, t, 1, 6, 4);
        CHECK(order);

        // merged tiles: one file the size of the mosaic
        o.mergeTiles = true;
        o.summary = false;
        res = exportLif(idx, {item}, out, o);
        const QString merged = QDir(res.folder).filePath(QStringLiteral("Folder A - stack.tif"));
        CHECK(res.failed.isEmpty() && res.written == QStringList{merged});
        CHECK(readTiffPages(merged, pages, desc, w, h, bits) && w == 10 + W && h == 1 + H && pages.size() == C * Z * T);

        // pictures: every z and t of the merged mosaic, then a projection per time point
        o.kind = LifExportOptions::AsShown;
        o.format = FileFormat::Png;
        o.planes = LifExportOptions::AllPlanes;
        res = exportLif(idx, {item}, dir.filePath(QStringLiteral("pictures")), o);
        CHECK(res.failed.isEmpty() && res.written.size() == Z * T);
        CHECK(res.written.contains(QDir(res.folder).filePath(QStringLiteral("Folder A - stack_z2_t1.png"))));
        QImage png(QDir(res.folder).filePath(QStringLiteral("Folder A - stack_z2_t1.png")));
        CHECK(png.width() == 10 + W && png.height() == 1 + H);
        o.planes = LifExportOptions::MaxProjection;
        res = exportLif(idx, {item}, dir.filePath(QStringLiteral("projections")), o);
        CHECK(res.failed.isEmpty() && res.written.size() == T
              && res.written.contains(QDir(res.folder).filePath(QStringLiteral("Folder A - stack_t2_max.png"))));
        // the current plane only, and a camera image exactly as recorded
        o.planes = LifExportOptions::CurrentPlane;
        o.format = FileFormat::Tiff;
        o.sixteenBit = true;
        LifExportItem camItem;
        camItem.image = 1;
        res = exportLif(idx, {item, camItem}, dir.filePath(QStringLiteral("current")), o);
        CHECK(res.failed.isEmpty() && res.written.size() == 2);
        LoadedImage li;
        CHECK(loadImage(QDir(res.folder).filePath(QStringLiteral("camera_ 10x_0.25.tif")), li, &err));
        CHECK(li.data.width == 4 && li.data.row(0)[0] == 200 * 257 && std::abs(li.meta.umPerPixel - 1.0) < 1e-9);
        // cancelling stops it
        o.kind = LifExportOptions::OriginalData;
        res = exportLif(idx, {item}, dir.filePath(QStringLiteral("cancelled")), o,
                        [](qint64 done, qint64, const QString &) { return done < 3; });
        CHECK(res.cancelled && res.written.isEmpty());
    }

    std::printf("lif: version 1 files, and three unnamed interleaved channels (older LAS)\n");
    {
        const QString v1 =
            QStringLiteral("<LMSDataContainerHeader Version=\"1\"><Element Name=\"old\"><Data><Image><ImageDescription>"
                           "<Channels><ChannelDescription Resolution=\"8\" BytesInc=\"0\"/>"
                           "<ChannelDescription Resolution=\"8\" BytesInc=\"1\"/><ChannelDescription Resolution=\"8\" "
                           "BytesInc=\"2\"/></Channels><Dimensions>")
            + dim(1, 4, 3, QStringLiteral("0")) + dim(2, 3, 12, QStringLiteral("0"))
            + QStringLiteral("</Dimensions></ImageDescription></Image></Data><Memory Size=\"36\" MemoryBlockID=\"B1\"/>"
                             "</Element></LMSDataContainerHeader>");
        const QString oldPath = dir.filePath(QStringLiteral("old.lif"));
        CHECK(writeFile(oldPath, buildLif(v1, {{QStringLiteral("B1"), camera}}, true)));
        LifFileIndex o;
        CHECK(readLif(oldPath, o, &err) && o.version == 1 && o.images.size() == 1);
        if (o.images.size() == 1) {
            CHECK(o.images[0].colour && o.images[0].umPerPixel == 0);
            LoadedImage li;
            CHECK(readLifImage(oldPath, o.images[0], li, &err) && li.data.row(0)[0] == 200 * 257);
        }
    }

    std::printf("lif: a file that is not a lif is refused, not crashed on\n");
    {
        const QString bogus = dir.filePath(QStringLiteral("not-a.lif"));
        CHECK(writeFile(bogus, QByteArray(4096, 'x')));
        QList<LifEntry> index;
        CHECK(!readLifIndex(bogus, index, &err));
        CHECK(!err.isEmpty());
        QList<LifEntry> idx2;
        CHECK(!readLifIndex(dir.filePath(QStringLiteral("missing.lif")), idx2, &err));
        // a valid header whose image data is cut off
        QByteArray cut = buildLif(xml, {{QStringLiteral("MemBlock_0"), QByteArray()},
                                        {QStringLiteral("MemBlock_1"), QByteArray()},
                                        {QStringLiteral("MemBlock_2"), stack}});
        cut.chop(100);
        const QString cutPath = dir.filePath(QStringLiteral("cut.lif"));
        CHECK(writeFile(cutPath, cut));
        LifFileIndex ci;
        CHECK(!readLif(cutPath, ci, &err) && !err.isEmpty());
        // random damage: never a crash
        std::mt19937 rng(7);
        const QByteArray good = buildLif(xml, {{QStringLiteral("MemBlock_0"), QByteArray()},
                                               {QStringLiteral("MemBlock_1"), QByteArray()},
                                               {QStringLiteral("MemBlock_2"), stack},
                                               {QStringLiteral("MemBlock_3"), QByteArray()},
                                               {QStringLiteral("MemBlock_4"), camera}});
        for (int round = 0; round < 200; ++round) {
            QByteArray bad = good;
            for (int k = 0; k < 8; ++k)
                bad[int(rng() % quint32(bad.size()))] = char(rng() & 0xff);
            const QString p = dir.filePath(QStringLiteral("fuzz.lif"));
            writeFile(p, bad);
            LifFileIndex fi;
            if (readLif(p, fi, &err))
                for (const LifEntry &fe : fi.images) {
                    LifPlane fp;
                    LifRequest fr;
                    fr.mergeTiles = true;
                    readLifPlane(p, fe, 0, fr, fp, &err);
                }
        }
    }

    // Optional: a real LAS X file, when one is available on this machine.
    if (const QByteArray real = qgetenv("DMI_LIF_SAMPLE"); !real.isEmpty()) {
        std::printf("lif: reading a real LAS X file\n");
        LifFileIndex ri;
        if (readLif(QString::fromLocal8Bit(real), ri, &err)) {
            std::printf("  %lld images\n", (long long)ri.images.size());
            CHECK(!ri.images.isEmpty());
            for (const LifEntry &re : ri.images) {
                CHECK(re.width > 0 && re.height > 0);
                LifPlane rp;
                CHECK(readLifPlane(QString::fromLocal8Bit(real), re, 0, LifRequest(), rp, &err));
                CHECK(rp.width == re.width);
            }
        } else {
            std::printf("  could not read: %s\n", qPrintable(err));
            ++g_failed;
        }
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QImageReader::setAllocationLimit(4096); // as in the app (Qt 6 default is 256 MB)
    QTemporaryDir dir;
    Image16 img(257, 131);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            for (int c = 0; c < 3; ++c)
                img.row(y)[x * 3 + c] = uint16_t((x * 251 + y * 97 + c * 4000) & 0xFFFF);
    ImageMetadata meta;
    meta.umPerPixel = 0.2093;
    meta.objective = QStringLiteral("N PLAN 40x/0.65");
    meta.notes = QStringLiteral("Unicode ✓ µm ß");
    meta.captureMode = QStringLiteral("hdr-3");
    meta.exposureMs = 10;
    meta.exposureSeriesMs = {10, 40, 160};

    std::printf("TIFF round trip\n");
    for (bool sixteen : {true, false})
        for (bool compress : {true, false}) {
            SaveOptions opt;
            opt.format = FileFormat::Tiff;
            opt.sixteenBit = sixteen;
            opt.compress = compress;
            const QString p = dir.filePath(QStringLiteral("t_%1_%2.tif").arg(sixteen).arg(compress));
            QString err;
            CHECK(saveImage(p, img, meta, opt, &err));
            LoadedImage li;
            CHECK(loadImage(p, li, &err));
            CHECK(li.data.width == img.width && li.data.height == img.height);
            CHECK(li.hasMeta && li.meta.objective == meta.objective && li.meta.notes == meta.notes);
            CHECK(li.meta.exposureSeriesMs == meta.exposureSeriesMs);
            CHECK(std::abs(li.meta.umPerPixel - meta.umPerPixel) < 1e-6);
            int maxDiff = 0;
            for (size_t i = 0; i < img.px.size() && li.data.px.size() == img.px.size(); ++i) {
                const int expect = sixteen ? img.px[i] : ((img.px[i] + 128) / 257) * 257;
                maxDiff = std::max(maxDiff, std::abs(int(li.data.px[i]) - expect));
            }
            CHECK(maxDiff == 0);
        }

    std::printf("TIFF calibration\n");
    {
        // uncalibrated round trip must stay uncalibrated (was 352.8 um/px from a 72 dpi tag)
        for (bool compress : {true, false}) {
            ImageMetadata m0 = meta;
            m0.umPerPixel = 0;
            SaveOptions opt;
            opt.format = FileFormat::Tiff;
            opt.compress = compress;
            const QString p = dir.filePath(QStringLiteral("uncal_%1.tif").arg(compress));
            QString err;
            CHECK(saveImage(p, img, m0, opt, &err));
            LoadedImage li;
            CHECK(loadImage(p, li, &err));
            CHECK(li.hasMeta && li.meta.umPerPixel == 0);
            Image16 raw;
            int bits = 0;
            QString desc;
            double um = -1;
            CHECK(readTiff(p, raw, bits, desc, um, &err) && um == 0);
        }
        // calibrated round trip (low magnification value too)
        for (double cal : {0.2093, 2.6, 0.0651}) {
            ImageMetadata m1 = meta;
            m1.umPerPixel = cal;
            SaveOptions opt;
            opt.format = FileFormat::Tiff;
            const QString p = dir.filePath(QStringLiteral("cal.tif"));
            QString err;
            CHECK(saveImage(p, img, m1, opt, &err));
            LoadedImage li;
            CHECK(loadImage(p, li, &err));
            CHECK(std::abs(li.meta.umPerPixel - cal) < 1e-9);
            Image16 raw;
            int bits = 0;
            QString desc;
            double um = 0;
            CHECK(readTiff(p, raw, bits, desc, um, &err) && std::abs(um - cal) / cal < 1e-4);
        }
        // files written by older versions: uncalibrated JSON + 72 dpi stored as px/cm
        ImageMetadata m0 = meta;
        m0.umPerPixel = 0;
        QString err;
        const QString legacy = dir.filePath(QStringLiteral("legacy.tif"));
        CHECK(writeTiff(legacy, img, 8, true, 2.54 / 72.0 * 10000.0, m0.toJsonString(), &err));
        LoadedImage li;
        CHECK(loadImage(legacy, li, &err));
        CHECK(li.hasMeta && li.meta.umPerPixel == 0);
        // ... and the same tag without any metadata is still not a calibration
        const QString legacyNoMeta = dir.filePath(QStringLiteral("legacy_nometa.tif"));
        CHECK(writeTiff(legacyNoMeta, img, 8, true, 2.54 / 72.0 * 10000.0, QString(), &err));
        CHECK(loadImage(legacyNoMeta, li, &err));
        CHECK(!li.hasMeta && li.meta.umPerPixel == 0);
        // JSON calibration wins over a (different) resolution tag
        ImageMetadata m1 = meta;
        m1.umPerPixel = 0.2093;
        const QString mism = dir.filePath(QStringLiteral("mismatch.tif"));
        CHECK(writeTiff(mism, img, 8, true, 1.0, m1.toJsonString(), &err));
        CHECK(loadImage(mism, li, &err));
        CHECK(std::abs(li.meta.umPerPixel - 0.2093) < 1e-9);
        // foreign TIFF (no DM Imaging metadata) with a physical calibration: use the tag
        const QString foreign = dir.filePath(QStringLiteral("foreign.tif"));
        CHECK(writeTiff(foreign, img, 8, true, 0.5, QString(), &err));
        CHECK(loadImage(foreign, li, &err));
        CHECK(std::abs(li.meta.umPerPixel - 0.5) < 1e-4);
    }

    std::printf("TIFF 4 GB limit\n");
    CHECK(tiffUncompressedSize(257, 131, 16) < kTiffMaxBytes);
    CHECK(tiffUncompressedSize(20000, 20000, 8) < kTiffMaxBytes);   // 1.2 GB
    CHECK(tiffUncompressedSize(30000, 30000, 8) < kTiffMaxBytes);   // 2.7 GB
    CHECK(tiffUncompressedSize(30000, 30000, 16) > kTiffMaxBytes);  // 5.4 GB
    CHECK(tiffUncompressedSize(40000, 40000, 8) > kTiffMaxBytes);   // 4.8 GB
    CHECK(tiffUncompressedSize(37837, 37837, 8) > kTiffMaxBytes);   // just over 2^32 incl. headers

    std::printf("PNG / JPEG round trip\n");
    for (auto fmt : {FileFormat::Png, FileFormat::Jpeg}) {
        SaveOptions opt;
        opt.format = fmt;
        opt.sixteenBit = fmt == FileFormat::Png;
        const QString p = dir.filePath(QStringLiteral("t.%1").arg(extensionFor(fmt)));
        QString err;
        CHECK(saveImage(p, img, meta, opt, &err));
        LoadedImage li;
        CHECK(loadImage(p, li, &err));
        CHECK(li.data.width == img.width && li.hasMeta);
    }

    std::printf("corrupted TIFF files (fuzz)\n");
    QFile f(dir.filePath(QStringLiteral("t_1_1.tif")));
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray good = f.readAll();
    f.close();
    std::mt19937 rng(42);
    int rejected = 0, loaded = 0;
    for (int i = 0; i < 2000; ++i) {
        QByteArray bad = good;
        const int mode = i % 4;
        if (mode == 0) { // random byte flips
            for (int k = 0; k < 1 + int(rng() % 20); ++k)
                bad[int(rng() % bad.size())] = char(rng());
        } else if (mode == 1) { // truncation
            bad.truncate(int(rng() % bad.size()));
        } else if (mode == 2) { // corrupt the IFD region (end of file)
            for (int k = 0; k < 30; ++k)
                bad[bad.size() - 1 - int(rng() % std::min<qsizetype>(600, bad.size()))] = char(rng());
        } else { // random header + garbage
            bad = QByteArray("II*\0", 4) + QByteArray(int(rng() % 300), char(rng()));
            for (auto &b : bad)
                if (rng() % 3 == 0)
                    b = char(rng());
        }
        const QString p = dir.filePath(QStringLiteral("bad.tif"));
        QFile o(p);
        CHECK(o.open(QIODevice::WriteOnly));
        o.write(bad);
        o.close();
        LoadedImage li;
        QString err;
        if (loadImage(p, li, &err))
            ++loaded;
        else
            ++rejected;
        ImageMetadata m;
        loadMetadata(p, m);
    }
    std::printf("  %d loaded, %d rejected, no crash\n", loaded, rejected);

    // ---- correcting the pixel size of images already saved
    std::printf("calibration repair: the recorded scale is corrected, the pixels are not\n");
    {
        QTemporaryDir rdir;
        Image16 small(24, 16);
        for (int y = 0; y < small.height; ++y)
            for (int x = 0; x < small.width * 3; ++x)
                small.row(y)[x] = uint16_t((x * 811 + y * 313) & 0xFFFF);

        // an image as it was saved with the assumed 0.7x adapter
        ImageMetadata wrong;
        wrong.magnification = 40;
        wrong.adapterFactor = 0.7;
        wrong.umPerPixel = 5.86 / (40 * 0.7); // 0.2093
        wrong.bitDepth = 16;
        SaveOptions so;
        so.format = FileFormat::Tiff;
        so.sixteenBit = true;
        const QString tif = rdir.filePath(QStringLiteral("wrong.tif"));
        QString err;
        CHECK(saveImage(tif, small, wrong, so, &err));

        // one that was taken with the right adapter must be left alone
        ImageMetadata right = wrong;
        right.adapterFactor = 1.0;
        right.umPerPixel = 5.86 / 40.0;
        const QString ok = rdir.filePath(QStringLiteral("already-right.tif"));
        CHECK(saveImage(ok, small, right, so, &err));

        auto fixes = findCalibrationFixes(rdir.path(), false, 0.7, 1.0);
        CHECK(fixes.size() == 1);
        if (fixes.size() == 1) {
            CHECK(QFileInfo(fixes[0].path).fileName() == QLatin1String("wrong.tif"));
            CHECK(std::abs(fixes[0].newUmPerPixel - 5.86 / 40.0) < 1e-6);
            CHECK(applyCalibrationFix(fixes[0], 1.0, &err));

            // the file now reports the corrected scale ...
            LoadedImage li;
            CHECK(loadImage(tif, li, &err));
            CHECK(std::abs(li.meta.umPerPixel - 5.86 / 40.0) < 1e-4);
            CHECK(std::abs(li.meta.adapterFactor - 1.0) < 1e-6);
            // ... in the resolution tags as well, which is what ImageJ reads
            Image16 back;
            int bits = 0;
            QString desc;
            double umpp = 0;
            CHECK(readTiff(tif, back, bits, desc, umpp, &err));
            CHECK(std::abs(umpp - 5.86 / 40.0) < 1e-4);
            CHECK(bits == 16);
            // ... and the pixels are untouched
            CHECK(back.width == small.width && back.height == small.height);
            int worst = 0;
            for (int y = 0; y < small.height; ++y)
                for (int x = 0; x < small.width * 3; ++x)
                    worst = std::max(worst, std::abs(int(back.row(y)[x]) - int(small.row(y)[x])));
            CHECK(worst == 0);

            // running it again finds nothing left to do
            CHECK(findCalibrationFixes(rdir.path(), false, 0.7, 1.0).isEmpty());
        }
    }

    std::printf("calibration repair: only the nominal scale is corrected, in every format, once\n");
    {
        QTemporaryDir rdir;
        Image16 small(24, 16);
        for (int y = 0; y < small.height; ++y)
            for (int x = 0; x < small.width * 3; ++x)
                small.row(y)[x] = uint16_t((x * 811 + y * 313) & 0xFFFF);
        const double nominalWrong = 5.86 / (40 * 0.7), nominalRight = 5.86 / 40.0;
        ImageMetadata wrong;
        wrong.magnification = 40;
        wrong.adapterFactor = 0.7;
        wrong.umPerPixel = nominalWrong;
        QString err;
        auto save = [&](const QString &name, FileFormat fmt, const ImageMetadata &m, bool sidecar = true) {
            SaveOptions so;
            so.format = fmt;
            so.sixteenBit = fmt != FileFormat::Jpeg;
            so.writeSidecar = sidecar;
            const QString p = rdir.filePath(name);
            CHECK(saveImage(p, small, m, so, &err));
            return p;
        };
        const QString png = save(QStringLiteral("wrong.png"), FileFormat::Png, wrong);
        const QString pngNoSc = save(QStringLiteral("wrong-nosidecar.png"), FileFormat::Png, wrong, false);
        const QString jpg = save(QStringLiteral("wrong.jpg"), FileFormat::Jpeg, wrong);
        const QString jpgNoSc = save(QStringLiteral("wrong-nosidecar.jpg"), FileFormat::Jpeg, wrong, false);
        // 16-shot pixel shift: half the nominal size
        ImageMetadata shifted = wrong;
        shifted.umPerPixel = nominalWrong / 2;
        shifted.captureMode = QStringLiteral("pixelshift-16");
        const QString shiftTif = save(QStringLiteral("shifted.tif"), FileFormat::Tiff, shifted);
        // a resized export of a nominal image: labelled nominal, at no whole upscale
        // of it, so corrected by the adapter ratio. Without the label (older files)
        // it cannot be told from a calibrated one and is left alone.
        ImageMetadata resized = wrong;
        resized.umPerPixel = nominalWrong * 2.5;
        resized.pixelSizeSource = QString::fromLatin1(kPixelSizeNominal);
        const QString resizedTif = save(QStringLiteral("resized.tif"), FileFormat::Tiff, resized);
        ImageMetadata resizedOld = resized;
        resizedOld.pixelSizeSource.clear();
        const QString resizedOldTif = save(QStringLiteral("resized-old.tif"), FileFormat::Tiff, resizedOld);
        auto expectedFor = [&](const QString &p) {
            return p == shiftTif ? nominalRight / 2 : p == resizedTif ? nominalWrong * 2.5 * 0.7 : nominalRight;
        };

        // images whose scale was measured or set by hand record the 0.7 adapter too
        ImageMetadata micro = wrong;
        micro.umPerPixel = 0.1471;
        micro.pixelSizeSource = QString::fromLatin1(kPixelSizeCalibrated);
        const QString microTif = save(QStringLiteral("micrometer.tif"), FileFormat::Tiff, micro);
        ImageMetadata microOld = micro; // an older file: source not recorded, measured value
        microOld.pixelSizeSource.clear();
        const QString microOldPng = save(QStringLiteral("micrometer-old.png"), FileFormat::Png, microOld);
        ImageMetadata manual = wrong; // hand-entered, and happens to equal the nominal value
        manual.pixelSizeSource = QString::fromLatin1(kPixelSizeManual);
        const QString manualTif = save(QStringLiteral("manual.tif"), FileFormat::Tiff, manual);

        QList<CalibrationFix> leftAlone;
        auto fixes = findCalibrationFixes(rdir.path(), false, 0.7, 1.0, &leftAlone);
        CHECK(fixes.size() == 6);
        CHECK(leftAlone.size() == 4);
        for (const CalibrationFix &fix : leftAlone) {
            CHECK(!fix.note.isEmpty());
            CHECK(fix.path == microTif || fix.path == microOldPng || fix.path == manualTif || fix.path == resizedOldTif);
            CHECK(!applyCalibrationFix(fix, 1.0, &err)); // never applied
        }
        for (const CalibrationFix &fix : fixes) {
            CHECK(fix.path != microTif && fix.path != microOldPng && fix.path != manualTif && fix.path != resizedOldTif);
            const double expect = expectedFor(fix.path);
            CHECK(std::abs(fix.newUmPerPixel - expect) < 1e-9);
            CHECK(fix.rewritesFile == !fix.path.endsWith(QLatin1String(".jpg")));
            CHECK(fix.createsSidecar == (fix.path == jpgNoSc));
            const bool applied = applyCalibrationFix(fix, 1.0, &err);
            if (!applied)
                std::printf("  %s: %s\n", qPrintable(fix.path), qPrintable(err));
            CHECK(applied);
            // applying the same fix twice changes nothing more
            CHECK(applyCalibrationFix(fix, 1.0, &err));
        }

        for (const QString &p : {png, pngNoSc, jpg, jpgNoSc, shiftTif, resizedTif}) {
            const double expect = expectedFor(p);
            LoadedImage li;
            CHECK(loadImage(p, li, &err));
            CHECK(std::abs(li.meta.umPerPixel - expect) < 1e-9);
            CHECK(std::abs(li.meta.adapterFactor - 1.0) < 1e-9);
            CHECK(li.meta.pixelSizeSource == QLatin1String(kPixelSizeNominal));
            ImageMetadata m;
            CHECK(loadMetadata(p, m));
            CHECK(std::abs(m.umPerPixel - expect) < 1e-9);
        }
        // a PNG's own metadata and density are corrected, not just its sidecar
        for (const QString &p : {png, pngNoSc}) {
            ImageMetadata em;
            CHECK(loadEmbeddedMetadata(p, em));
            CHECK(std::abs(em.umPerPixel - nominalRight) < 1e-9);
            QImageReader r(p);
            const QImage q = r.read();
            CHECK(q.dotsPerMeterX() == int(std::lround(1e6 / nominalRight)));
        }
        CHECK(!QFile::exists(sidecarPath(pngNoSc)));
        // the JPEG without a sidecar has one now, and it is what the program reads
        CHECK(QFile::exists(sidecarPath(jpgNoSc)));

        // the files left alone are untouched
        LoadedImage li;
        CHECK(loadImage(microTif, li, &err) && std::abs(li.meta.umPerPixel - 0.1471) < 1e-12);
        CHECK(std::abs(li.meta.adapterFactor - 0.7) < 1e-12);
        CHECK(loadImage(microOldPng, li, &err) && std::abs(li.meta.umPerPixel - 0.1471) < 1e-12);

        // a second scan finds nothing to correct (only the ones left alone)
        leftAlone.clear();
        CHECK(findCalibrationFixes(rdir.path(), false, 0.7, 1.0, &leftAlone).isEmpty());
        CHECK(leftAlone.size() == 4);

        // a repair interrupted after the file but before its sidecar is finished
        // by the next scan, without correcting the file a second time
        const QString half = save(QStringLiteral("half.png"), FileFormat::Png, wrong);
        fixes = findCalibrationFixes(rdir.path(), false, 0.7, 1.0);
        CHECK(fixes.size() == 1);
        if (fixes.size() == 1) {
            QFile scFile(sidecarPath(half));
            CHECK(scFile.open(QIODevice::ReadOnly));
            const QByteArray staleSidecar = scFile.readAll();
            scFile.close();
            CHECK(applyCalibrationFix(fixes[0], 1.0, &err));
            CHECK(scFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
            scFile.write(staleSidecar);
            scFile.close();
            fixes = findCalibrationFixes(rdir.path(), false, 0.7, 1.0);
            CHECK(fixes.size() == 1 && std::abs(fixes[0].newUmPerPixel - nominalRight) < 1e-9);
            if (fixes.size() == 1)
                CHECK(applyCalibrationFix(fixes[0], 1.0, &err));
            ImageMetadata em;
            CHECK(loadEmbeddedMetadata(half, em) && std::abs(em.umPerPixel - nominalRight) < 1e-9);
            CHECK(loadMetadata(half, em) && std::abs(em.umPerPixel - nominalRight) < 1e-9);
            CHECK(findCalibrationFixes(rdir.path(), false, 0.7, 1.0).isEmpty());
        }
    }

    std::printf("sidecar pixel size wins in loadImage as in loadMetadata\n");
    {
        ImageMetadata m = meta;
        m.umPerPixel = 0.3;
        SaveOptions so;
        so.format = FileFormat::Png;
        const QString p = dir.filePath(QStringLiteral("precedence.png"));
        QString err;
        CHECK(saveImage(p, img, m, so, &err));
        ImageMetadata sc;
        CHECK(loadSidecarMetadata(p, sc));
        sc.umPerPixel = 0.2;
        QFile scf(sidecarPath(p));
        CHECK(scf.open(QIODevice::WriteOnly | QIODevice::Truncate));
        scf.write(sc.toJsonString().toUtf8());
        scf.close();
        LoadedImage li;
        CHECK(loadImage(p, li, &err) && std::abs(li.meta.umPerPixel - 0.2) < 1e-12);
        CHECK(li.meta.notes == meta.notes); // the rest still comes from the file
        ImageMetadata lm2;
        CHECK(loadMetadata(p, lm2) && std::abs(lm2.umPerPixel - 0.2) < 1e-12);
    }

    lifTests(dir);

    std::printf("\n%s (%d failures)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
