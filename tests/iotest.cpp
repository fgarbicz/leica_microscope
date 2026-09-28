// Image I/O tests: TIFF round trip (8/16 bit, compression, calibration and
// metadata), robustness against corrupted files (fuzzing), the calibration
// repair in every format, and .lif read/write.
#include "io/ImageIO.h"
#include "io/CalibrationRepair.h"
#include "io/LifFile.h"

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
        for (const CalibrationFix &f : leftAlone) {
            CHECK(!f.note.isEmpty());
            CHECK(f.path == microTif || f.path == microOldPng || f.path == manualTif || f.path == resizedOldTif);
            CHECK(!applyCalibrationFix(f, 1.0, &err)); // never applied
        }
        for (const CalibrationFix &f : fixes) {
            CHECK(f.path != microTif && f.path != microOldPng && f.path != manualTif && f.path != resizedOldTif);
            const double expect = expectedFor(f.path);
            CHECK(std::abs(f.newUmPerPixel - expect) < 1e-9);
            CHECK(f.rewritesFile == !f.path.endsWith(QLatin1String(".jpg")));
            CHECK(f.createsSidecar == (f.path == jpgNoSc));
            const bool applied = applyCalibrationFix(f, 1.0, &err);
            if (!applied)
                std::printf("  %s: %s\n", qPrintable(f.path), qPrintable(err));
            CHECK(applied);
            // applying the same fix twice changes nothing more
            CHECK(applyCalibrationFix(f, 1.0, &err));
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

    // ---- Leica .lif container
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
            CHECK(index[0].channels == 3);
            CHECK(std::abs(index[0].umPerPixel - 0.2929) < 1e-4);
            CHECK(std::abs(index[1].umPerPixel - 0.0732) < 1e-4);

            LoadedImage li;
            CHECK(readLifImage(lifPath, index[0], li, &err));
            CHECK(li.data.width == 37 && li.data.height == 19);
            CHECK(std::abs(li.meta.umPerPixel - 0.2929) < 1e-4);
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
            CHECK(index[0].bitsPerSample == 16 && index[0].bytesPerPixel == 6);
            CHECK(index[0].redOffset == 4 && index[0].greenOffset == 2 && index[0].blueOffset == 0);
            LoadedImage li;
            CHECK(readLifImage(lifPath, index[0], li, &err));
            CHECK(li.data.width == c.width && li.data.height == c.height && li.data.px == c.px);

            // offsets outside the pixel or a row wider than its stride are refused
            LifEntry bad = index[0];
            bad.redOffset = 5; // + 2 bytes > 6
            CHECK(!readLifImage(lifPath, bad, li, &err) && !err.isEmpty());
            bad = index[0];
            bad.rowStride = bad.width * bad.bytesPerPixel - 1;
            CHECK(!readLifImage(lifPath, bad, li, &err));
            bad = index[0];
            bad.width = bad.height = 1 << 28; // claims far more than the file holds
            bad.rowStride = qint64(bad.width) * bad.bytesPerPixel;
            CHECK(!readLifImage(lifPath, bad, li, &err));
            LoadedImage viaLoad;
            CHECK(loadImage(lifPath, viaLoad, &err) && viaLoad.data.px == c.px);

            // Planar channels (a fluorescence image stores each as a plane of its
            // own): the same data block read as three planes of w*h samples, as
            // LAS X describes such a file - channel offsets a plane apart.
            QFile raw(lifPath);
            CHECK(raw.open(QIODevice::ReadOnly) && raw.seek(index[0].dataOffset));
            const QByteArray block = raw.read(index[0].dataBytes);
            raw.close();
            const qint64 plane = qint64(c.width) * c.height * 2;
            CHECK(block.size() == 3 * plane);
            auto rawSample = [&](qint64 byteOffset) {
                const auto *b = reinterpret_cast<const uchar *>(block.constData()) + byteOffset;
                return uint16_t(b[0] | b[1] << 8);
            };
            LifEntry planar = index[0];
            planar.bytesPerPixel = 2;
            planar.rowStride = qint64(c.width) * 2;
            planar.redOffset = 0;
            planar.greenOffset = plane;
            planar.blueOffset = 2 * plane;
            LoadedImage pl;
            CHECK(readLifImage(lifPath, planar, pl, &err));
            bool planesMatch = pl.data.width == c.width && pl.data.height == c.height;
            for (int y = 0; planesMatch && y < c.height; ++y)
                for (int x = 0; x < c.width; ++x)
                    for (int ch = 0; ch < 3; ++ch)
                        if (pl.data.row(y)[3 * x + ch] != rawSample(ch * plane + (qint64(y) * c.width + x) * 2))
                            planesMatch = false;
            CHECK(planesMatch);
            // a mono image shows its first channel, wherever that is
            LifEntry mono = planar;
            mono.colour = false;
            mono.firstOffset = plane; // the second plane
            CHECK(readLifImage(lifPath, mono, pl, &err));
            CHECK(pl.data.row(3)[3 * 5] == rawSample(plane + (3 * qint64(c.width) + 5) * 2)
                  && pl.data.row(3)[3 * 5 + 1] == pl.data.row(3)[3 * 5]);
            // a plane that would run past the data block is refused
            planar.blueOffset = 2 * plane + 2;
            CHECK(!readLifImage(lifPath, planar, pl, &err) && !err.isEmpty());
        }
    }

    std::printf("lif: a file that is not a lif is refused, not crashed on\n");
    {
        const QString bogus = dir.filePath(QStringLiteral("not-a.lif"));
        QFile bf(bogus);
        CHECK(bf.open(QIODevice::WriteOnly));
        bf.write(QByteArray(4096, 'x'));
        bf.close();
        QList<LifEntry> index;
        QString err;
        CHECK(!readLifIndex(bogus, index, &err));
        CHECK(!err.isEmpty());
        // truncated after a valid header
        QList<LifEntry> idx2;
        CHECK(!readLifIndex(dir.filePath(QStringLiteral("missing.lif")), idx2, &err));
    }

    // Optional: a real LAS X file, when one is available on this machine.
    if (const QByteArray real = qgetenv("DMI_LIF_SAMPLE"); !real.isEmpty()) {
        std::printf("lif: reading a real LAS X file\n");
        QList<LifEntry> index;
        QString err;
        if (readLifIndex(QString::fromLocal8Bit(real), index, &err)) {
            std::printf("  %lld images\n", (long long)index.size());
            CHECK(!index.isEmpty());
            for (const LifEntry &e : index) {
                CHECK(e.width > 0 && e.height > 0);
                CHECK(e.dataBytes >= e.rowStride * e.height);
            }
            LoadedImage li;
            CHECK(readLifImage(QString::fromLocal8Bit(real), index.first(), li, &err));
            CHECK(li.data.width == index.first().width);
        } else {
            std::printf("  could not read: %s\n", qPrintable(err));
            ++g_failed;
        }
    }

    std::printf("\n%s (%d failures)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
