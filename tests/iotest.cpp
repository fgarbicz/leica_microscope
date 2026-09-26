// Image I/O tests: TIFF round trip (8/16 bit, compression, calibration and
// metadata) and robustness against corrupted files (fuzzing).
#include "io/ImageIO.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QTemporaryDir>

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
        o.open(QIODevice::WriteOnly);
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

    std::printf("\n%s (%d failures)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
