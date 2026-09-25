// Image I/O tests: TIFF round trip (8/16 bit, compression, calibration and
// metadata) and robustness against corrupted files (fuzzing).
#include "io/ImageIO.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
            CHECK(std::abs(li.meta.umPerPixel - meta.umPerPixel) < 1e-6);
            int maxDiff = 0;
            for (size_t i = 0; i < img.px.size() && li.data.px.size() == img.px.size(); ++i) {
                const int expect = sixteen ? img.px[i] : ((img.px[i] + 128) / 257) * 257;
                maxDiff = std::max(maxDiff, std::abs(int(li.data.px[i]) - expect));
            }
            CHECK(maxDiff == 0);
        }

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
