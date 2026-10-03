// Command-line look into a Leica .lif, and its export without the interface:
//
//   lifinfo file.lif                        the tree and every image
//   lifinfo file.lif --xml N                the LAS X metadata of image N
//   lifinfo file.lif --raw N C out.raw [i…] one channel of one plane, uint16 little endian
//                                           (i… = an index per dimension beyond x and y)
//   lifinfo file.lif --mosaic N C out.raw   the same, tiles merged
//   lifinfo file.lif --export DIR [--shown] [--max] [--tiles] [--png|--jpeg] [--images 0,2,5]
//                                           ImageJ hyperstacks (default) or pictures as shown
//
// Images are numbered from 0 in the order listed.
#include "io/LifExport.h"
#include "io/LifFile.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>

#include <cstdio>
#include <cstring>

using namespace lm;

namespace {

void printTree(const LifFileIndex &idx, const LifNode &n, int depth)
{
    const QString indent(depth * 2, QLatin1Char(' '));
    if (n.image >= 0) {
        const LifEntry &e = idx.images[n.image];
        std::printf("%s[%d] %s  %s", qPrintable(indent), n.image, qPrintable(e.name), qPrintable(e.summary()));
        if (e.umPerPixel > 0)
            std::printf("  %.5g um/px", e.umPerPixel);
        std::printf("\n");
        for (const LifChannel &c : e.channels)
            std::printf("%s      ch %s: %s, %d-bit, inc %lld\n", qPrintable(indent), qPrintable(c.name),
                        qPrintable(c.lut), c.bits, (long long)c.bytesInc);
        for (const LifDimension &d : e.planeDims)
            std::printf("%s      %s: %d, inc %lld, step %.5g %s\n", qPrintable(indent), qPrintable(d.label()), d.size,
                        (long long)d.bytesInc, d.step(), qPrintable(d.unit));
        for (const auto &kv : e.info)
            std::printf("%s      %s: %s\n", qPrintable(indent), qPrintable(kv.first), qPrintable(kv.second));
    } else {
        std::printf("%s%s/\n", qPrintable(indent), qPrintable(n.name));
    }
    for (const LifNode &c : n.children)
        printTree(idx, c, depth + 1);
}

bool writeRaw(const QString &path, const LifPlane &p)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    QByteArray b(qsizetype(p.px.size()) * 2, Qt::Uninitialized);
    for (size_t i = 0; i < p.px.size(); ++i) {
        b[qsizetype(2 * i)] = char(p.px[i] & 0xff);
        b[qsizetype(2 * i + 1)] = char(p.px[i] >> 8);
    }
    return f.write(b) == b.size();
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv); // the exporter draws pictures
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        std::printf("usage: lifinfo file.lif [--xml N | --raw N C out.raw [i...] | --mosaic N C out.raw |\n"
                    "                         --export DIR [--shown] [--max] [--tiles] [--png|--jpeg] [--images a,b]]\n");
        return 1;
    }
    LifFileIndex idx;
    QString err;
    if (!readLif(args[1], idx, &err)) {
        std::fprintf(stderr, "%s\n", qPrintable(err));
        return 2;
    }
    const auto image = [&](int i) -> const LifEntry * {
        if (i < 0 || i >= idx.images.size()) {
            std::fprintf(stderr, "no image %d (the file has %lld)\n", i, (long long)idx.images.size());
            return nullptr;
        }
        return &idx.images[i];
    };
    if (args.size() == 2) {
        std::printf("%s: version %d, %lld images\n", qPrintable(args[1]), idx.version, (long long)idx.images.size());
        printTree(idx, idx.root, 0);
        for (const QString &p : idx.problems)
            std::printf("left out: %s\n", qPrintable(p));
        return 0;
    }
    const QString cmd = args[2];
    if (cmd == QLatin1String("--xml") && args.size() >= 4) {
        const LifEntry *e = image(args[3].toInt());
        if (!e)
            return 1;
        std::printf("%s\n", qPrintable(idx.elementXml(*e)));
        return 0;
    }
    if ((cmd == QLatin1String("--raw") || cmd == QLatin1String("--mosaic")) && args.size() >= 6) {
        const LifEntry *e = image(args[3].toInt());
        if (!e)
            return 1;
        LifRequest r;
        for (int i = 6; i < args.size(); ++i)
            r.coord << args[i].toInt();
        r.mergeTiles = cmd == QLatin1String("--mosaic");
        LifPlane p;
        if (!readLifPlane(args[1], *e, args[4].toInt(), r, p, &err)) {
            std::fprintf(stderr, "%s\n", qPrintable(err));
            return 2;
        }
        if (!writeRaw(args[5], p)) {
            std::fprintf(stderr, "cannot write %s\n", qPrintable(args[5]));
            return 2;
        }
        std::printf("%d x %d\n", p.width, p.height);
        return 0;
    }
    if (cmd == QLatin1String("--export") && args.size() >= 4) {
        LifExportOptions o;
        QList<int> which;
        for (int i = 4; i < args.size(); ++i) {
            const QString a = args[i];
            if (a == QLatin1String("--shown"))
                o.kind = LifExportOptions::AsShown;
            else if (a == QLatin1String("--max"))
                o.planes = LifExportOptions::MaxProjection;
            else if (a == QLatin1String("--tiles"))
                o.mergeTiles = false;
            else if (a == QLatin1String("--png"))
                o.format = FileFormat::Png;
            else if (a == QLatin1String("--jpeg"))
                o.format = FileFormat::Jpeg;
            else if (a == QLatin1String("--images") && i + 1 < args.size())
                for (const QString &n : args[++i].split(QLatin1Char(',')))
                    which << n.toInt();
        }
        if (which.isEmpty())
            for (int i = 0; i < idx.images.size(); ++i)
                which << i;
        QList<LifExportItem> items;
        for (int i : which) {
            LifExportItem it;
            it.image = i;
            items << it;
        }
        const LifExportResult r = exportLif(idx, items, args[3], o, [](qint64 done, qint64 total, const QString &) {
            std::fprintf(stderr, "\r%lld / %lld", (long long)done, (long long)total);
            return true;
        });
        std::fprintf(stderr, "\n");
        for (const QString &f : r.written)
            std::printf("wrote %s\n", qPrintable(f));
        for (const QString &f : r.failed)
            std::printf("FAILED %s\n", qPrintable(f));
        return r.failed.isEmpty() ? 0 : 3;
    }
    std::fprintf(stderr, "unknown command %s\n", qPrintable(cmd));
    return 1;
}
