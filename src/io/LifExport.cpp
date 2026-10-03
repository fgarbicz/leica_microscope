#include "LifExport.h"

#include "io/ImageJTiff.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace lm {

namespace {

// What fits in one classic TIFF with room to spare for the tags.
constexpr qint64 kMaxStackBytes = 3800LL * 1024 * 1024;

// Every combination of the indices of `dims` (planeDims indices), the first
// varying fastest; `base` supplies the other indices.
QList<LifCoord> combinations(const LifEntry &e, const LifCoord &base, const QList<int> &dims)
{
    LifCoord start = base;
    while (start.size() < e.planeDims.size())
        start.append(0);
    for (int d : dims)
        start[d] = 0;
    QList<LifCoord> out{start};
    for (int d : dims) {
        QList<LifCoord> next;
        for (int k = 0; k < e.planeDims[d].size; ++k)
            for (LifCoord c : out) {
                c[d] = k;
                next.append(c);
            }
        out = next;
    }
    return out;
}

// "_z03_t012": the indices (from 1) of the dimensions that vary between files.
QString suffixFor(const LifEntry &e, const LifCoord &c, const QList<int> &dims)
{
    QString s;
    for (int d : dims) {
        const LifDimension &dim = e.planeDims[d];
        const int digits = int(QString::number(dim.size).size());
        QString tag;
        switch (dim.id) {
        case LifDimZ: tag = QStringLiteral("z"); break;
        case LifDimT: tag = QStringLiteral("t"); break;
        case LifDimMosaic: tag = QStringLiteral("tile"); break;
        case LifDimLambda: tag = QStringLiteral("lambda"); break;
        case LifDimLambdaEx: tag = QStringLiteral("ex"); break;
        default: tag = QStringLiteral("d%1-").arg(dim.id);
        }
        s += QStringLiteral("_%1%2").arg(tag).arg(c.value(d) + 1, digits, 10, QLatin1Char('0'));
    }
    return s;
}

QString unique(const QString &folder, const QString &stem, const QString &ext, QSet<QString> &used)
{
    QString name = stem + ext;
    for (int n = 2; used.contains(name.toLower()); ++n)
        name = QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(ext);
    used.insert(name.toLower());
    return QDir(folder).filePath(name);
}

QString csvField(QString s)
{
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n')))
        s = QLatin1Char('"') + s.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
    return s;
}

} // namespace

QString safeFileName(const QString &name)
{
    QString s;
    for (const QChar ch : name) {
        // what Windows, macOS or Linux refuse in a file name
        if (ch.unicode() < 32 || QStringLiteral("\\/:*?\"<>|").contains(ch))
            s += QLatin1Char('_');
        else
            s += ch;
    }
    s = s.simplified();
    while (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    if (s.size() > 150)
        s = s.left(150).trimmed();
    return s.isEmpty() ? QStringLiteral("image") : s;
}

QString lifImageFileName(const LifEntry &e)
{
    // the first folder is the file itself
    QStringList parts = e.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty())
        parts.removeFirst();
    parts << e.name;
    return safeFileName(parts.join(QStringLiteral(" - ")));
}

QList<LifChannelDisplay> autoLifDisplay(const LifEntry &e, const QList<LifPlane> &planes)
{
    QList<LifChannelDisplay> d = defaultLifDisplay(e);
    if (e.colour)
        return d;
    for (int c = 0; c < d.size() && c < planes.size(); ++c)
        autoLifRange(planes[c], d[c].low, d[c].high);
    return d;
}

LifExportResult exportLif(const LifFileIndex &index, const QList<LifExportItem> &items, const QString &folderIn,
                          const LifExportOptions &opt, const LifExportProgress &progress)
{
    LifExportResult res;
    const QString lifPath = index.path;
    res.folder = opt.subfolder ? QDir(folderIn).filePath(safeFileName(QFileInfo(lifPath).completeBaseName()))
                               : folderIn;
    if (!QDir().mkpath(res.folder)) {
        res.failed << QObject::tr("Cannot create the folder %1").arg(QDir::toNativeSeparators(res.folder));
        return res;
    }

    // ---- the work, as a list of files, each with its planes
    struct Job {
        int item;
        QString path;
        QList<LifCoord> coords; // AsShown: one picture per coord; OriginalData: the file's planes (c fastest)
        int channels = 1, slices = 1, frames = 1;
    };
    QList<Job> jobs;
    QSet<QString> used;
    qint64 total = 0;
    const QString ext = opt.kind == LifExportOptions::OriginalData ? QStringLiteral(".tif")
                                                                    : QLatin1Char('.') + extensionFor(opt.format);
    for (int i = 0; i < items.size(); ++i) {
        const LifExportItem &it = items[i];
        if (it.image < 0 || it.image >= index.images.size())
            continue;
        const LifEntry &e = index.images[it.image];
        const QString stem = lifImageFileName(e);
        const int zi = e.dimIndex(LifDimZ), ti = e.dimIndex(LifDimT), mi = e.dimIndex(LifDimMosaic);
        const bool merge = opt.mergeTiles && mi >= 0 && e.planeDims[mi].size > 1;
        if (opt.kind == LifExportOptions::OriginalData) {
            // z and t go into the stack; everything else that varies makes files
            QList<int> perFile;
            for (int d = 0; d < e.planeDims.size(); ++d)
                if (d != zi && d != ti && e.planeDims[d].size > 1 && !(merge && d == mi))
                    perFile << d;
            const QSize size = merge ? lifMosaicSize(e) : QSize(e.width, e.height);
            int bits = 8;
            for (const LifChannel &c : e.channels)
                bits = std::max(bits, c.isFloat || c.sampleBytes > 1 ? 16 : 8);
            const int slices = zi >= 0 ? e.planeDims[zi].size : 1, frames = ti >= 0 ? e.planeDims[ti].size : 1;
            const qint64 perZ = imageJStackBytes(size.width(), size.height(), bits, e.channels.size());
            // too big for one TIFF: a file per time point, then per slice
            QList<int> inFile;
            if (perZ * slices * frames > kMaxStackBytes && ti >= 0 && frames > 1)
                perFile << ti;
            else if (ti >= 0)
                inFile << ti;
            if (perZ * slices * (perFile.contains(ti) ? 1 : frames) > kMaxStackBytes && zi >= 0 && slices > 1)
                perFile << zi;
            else if (zi >= 0)
                inFile.prepend(zi);
            for (const LifCoord &fileCoord : combinations(e, it.request.coord, perFile)) {
                Job j;
                j.item = i;
                j.path = unique(res.folder, stem + suffixFor(e, fileCoord, perFile), ext, used);
                j.channels = int(e.channels.size());
                j.slices = zi >= 0 && inFile.contains(zi) ? slices : 1;
                j.frames = ti >= 0 && inFile.contains(ti) ? frames : 1;
                j.coords = combinations(e, fileCoord, inFile); // z fastest, then t
                total += qint64(j.coords.size()) * j.channels;
                jobs.append(j);
            }
        } else {
            QList<int> vary;
            if (opt.planes == LifExportOptions::AllPlanes || opt.planes == LifExportOptions::MaxProjection)
                for (int d = 0; d < e.planeDims.size(); ++d)
                    if (e.planeDims[d].size > 1 && !(merge && d == mi)
                        && !(opt.planes == LifExportOptions::MaxProjection && d == zi))
                        vary << d;
            for (const LifCoord &c : combinations(e, it.request.coord, vary)) {
                Job j;
                j.item = i;
                QString suffix = suffixFor(e, c, vary);
                if (opt.planes == LifExportOptions::MaxProjection && zi >= 0 && e.planeDims[zi].size > 1)
                    suffix += QStringLiteral("_max");
                j.path = unique(res.folder, stem + suffix, ext, used);
                j.coords = {c};
                total += qint64(e.channels.size())
                         * (opt.planes == LifExportOptions::MaxProjection && zi >= 0 ? e.planeDims[zi].size : 1);
                jobs.append(j);
            }
        }
    }

    // ---- writing
    qint64 done = 0;
    std::atomic<bool> cancelFlag{false};
    const auto tick = [&](const QString &what, qint64 n = 1) {
        done += n;
        if (progress && !progress(done, total, what)) {
            res.cancelled = true;
            cancelFlag = true;
        }
        return !res.cancelled;
    };
    QHash<int, QList<LifChannelDisplay>> displays; // per item, decided once
    for (const Job &j : jobs) {
        if (res.cancelled)
            break;
        const LifExportItem &it = items[j.item];
        const LifEntry &e = index.images[it.image];
        const int zi = e.dimIndex(LifDimZ), mi = e.dimIndex(LifDimMosaic);
        const bool merge = opt.mergeTiles && mi >= 0 && e.planeDims[mi].size > 1;
        const QString label = QStringLiteral("%1 → %2").arg(e.name, QFileInfo(j.path).fileName());
        QString err;

        if (opt.kind == LifExportOptions::OriginalData) {
            const QSize size = merge ? lifMosaicSize(e) : QSize(e.width, e.height);
            int bits = 8;
            for (const LifChannel &c : e.channels)
                bits = std::max(bits, c.isFloat || c.sampleBytes > 1 ? 16 : 8);
            // the display ranges travel as ImageJ's; automatic ones from the first plane
            QList<LifChannelDisplay> disp = it.display;
            if (disp.size() != e.channels.size()) {
                QList<LifPlane> first;
                LifRequest r;
                r.coord = j.coords.value(0);
                r.mergeTiles = merge;
                disp = readLifChannels(lifPath, e, r, first, nullptr) ? autoLifDisplay(e, first) : defaultLifDisplay(e);
            }
            QList<StackChannel> luts;
            for (const LifChannelDisplay &d : disp)
                luts.append({d.colour, double(d.low), double(d.high)});
            StackCalibration cal;
            cal.umPerPixel = e.umPerPixel;
            cal.umPerPixelY = e.umPerPixelY;
            if (zi >= 0)
                cal.zStepUm = e.planeDims[zi].step();
            if (const int ti = e.dimIndex(LifDimT); ti >= 0)
                cal.frameIntervalS = e.planeDims[ti].step();
            const auto source = [&](int page, std::vector<uint16_t> &plane, QString *why) {
                const int c = page % j.channels;
                LifRequest r;
                r.coord = j.coords.value(page / j.channels);
                r.mergeTiles = merge;
                LifPlane p;
                if (!readLifPlane(lifPath, e, c, r, p, why, &cancelFlag))
                    return false;
                if (p.width != size.width() || p.height != size.height()) {
                    if (why)
                        *why = QObject::tr("A plane has a different size.");
                    return false;
                }
                plane.swap(p.px);
                if (!tick(label)) {
                    if (why)
                        *why = QObject::tr("Cancelled");
                    return false;
                }
                return true;
            };
            if (writeImageJStack(j.path, size.width(), size.height(), bits, j.channels, j.slices, j.frames, source, cal,
                                 luts, opt.compress, &err))
                res.written << j.path;
            else if (!res.cancelled)
                res.failed << QStringLiteral("%1: %2").arg(e.name, err);
            continue;
        }

        // AsShown: one picture
        LifRequest r = it.request;
        r.coord = j.coords.value(0);
        r.mergeTiles = merge;
        r.subsample = 1;
        r.projectDim = opt.planes == LifExportOptions::MaxProjection && zi >= 0 && e.planeDims[zi].size > 1 ? zi
                       : opt.planes == LifExportOptions::CurrentPlane                                    ? it.request.projectDim
                                                                                                         : -1;
        QList<LifPlane> planes;
        if (!readLifChannels(lifPath, e, r, planes, &err, &cancelFlag)) {
            res.failed << QStringLiteral("%1: %2").arg(e.name, err);
            continue;
        }
        if (!displays.contains(j.item))
            displays.insert(j.item, it.display.size() == e.channels.size() ? it.display : autoLifDisplay(e, planes));
        ImageMetadata meta = lifMetadata(e);
        meta.width = planes[0].width;
        meta.height = planes[0].height;
        meta.bitDepth = opt.sixteenBit ? 16 : 8;
        SaveOptions so;
        so.format = opt.format;
        so.sixteenBit = opt.sixteenBit;
        so.compress = opt.compress;
        so.jpegQuality = opt.jpegQuality;
        bool ok = false;
        try {
            const Image16 img = composeLif(planes, displays.value(j.item));
            planes.clear();
            if (opt.decorate) {
                meta.bitDepth = 8;
                ok = saveImage(j.path, opt.decorate(toQImage8(img), meta.umPerPixel), meta, so, &err);
            } else {
                ok = saveImage(j.path, img, meta, so, &err);
            }
        } catch (const std::bad_alloc &) {
            err = QObject::tr("Not enough memory");
        }
        if (ok)
            res.written << j.path;
        else
            res.failed << QStringLiteral("%1: %2").arg(e.name, err);
        tick(label, qint64(e.channels.size()) * (r.projectDim >= 0 ? e.planeDims[r.projectDim].size : 1));
    }

    // ---- the summary
    if (opt.summary && !res.cancelled) {
        const QString csvPath = QDir(res.folder).filePath(QStringLiteral("images.csv"));
        QSaveFile csv(csvPath);
        if (csv.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&csv);
            ts << "folder,image,width,height,channels,channel_names,bits,z,t,tiles,um_per_pixel,z_step_um,"
                  "t_interval_s,objective,numerical_aperture,acquired,files\n";
            QSet<int> seen;
            const QSet<QString> written(res.written.cbegin(), res.written.cend());
            for (int i = 0; i < items.size(); ++i) {
                const int idx = items[i].image;
                if (idx < 0 || idx >= index.images.size() || seen.contains(idx))
                    continue;
                seen.insert(idx);
                const LifEntry &e = index.images[idx];
                QStringList names, files;
                for (const LifChannel &c : e.channels)
                    names << c.name;
                for (const Job &j : jobs)
                    if (items[j.item].image == idx && written.contains(j.path))
                        files << QFileInfo(j.path).fileName();
                QStringList path = e.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
                if (!path.isEmpty())
                    path.removeFirst();
                const int zi = e.dimIndex(LifDimZ), ti = e.dimIndex(LifDimT);
                ts << csvField(path.join(QLatin1Char('/'))) << ',' << csvField(e.name) << ',' << e.width << ','
                   << e.height << ',' << e.channels.size() << ',' << csvField(names.join(QLatin1Char(';'))) << ','
                   << (e.channels.isEmpty() ? 8 : e.channels.first().bits) << ',' << e.sizeOf(LifDimZ) << ','
                   << e.sizeOf(LifDimT) << ',' << e.sizeOf(LifDimMosaic) << ','
                   << QString::number(e.umPerPixel, 'g', 8) << ','
                   << (zi >= 0 ? QString::number(e.planeDims[zi].step(), 'g', 8) : QString()) << ','
                   << (ti >= 0 ? QString::number(e.planeDims[ti].step(), 'g', 8) : QString()) << ','
                   << csvField(e.objective) << ','
                   << (e.numericalAperture > 0 ? QString::number(e.numericalAperture) : QString()) << ','
                   << (e.acquired.isValid() ? e.acquired.toString(Qt::ISODate) : QString()) << ','
                   << csvField(files.join(QLatin1Char(';'))) << '\n';
            }
            ts.flush();
            if (csv.commit())
                res.written << csvPath;
            else
                res.failed << QObject::tr("images.csv: %1").arg(csv.errorString());
        }
        const QString xmlPath =
            QDir(res.folder).filePath(safeFileName(QFileInfo(lifPath).completeBaseName()) + QStringLiteral("_metadata.xml"));
        QSaveFile xml(xmlPath);
        if (xml.open(QIODevice::WriteOnly)) {
            xml.write(index.xml.toUtf8());
            if (xml.commit())
                res.written << xmlPath;
        }
    }
    return res;
}

} // namespace lm
