#include "CalibrationRepair.h"

#include "io/ImageIO.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cmath>

namespace lm {

namespace {

const QStringList kImagePatterns = {QStringLiteral("*.tif"), QStringLiteral("*.tiff"), QStringLiteral("*.png"),
                                    QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.bmp")};

// Output pixels per sensor pixel a capture can record: 1, or 2 and 3 for 16-
// and 36-shot pixel shift (MainWindow divides the nominal size by it).
constexpr int kMaxUpscale = 3;
// The nominal size is stored as computed, so it matches to rounding; a stage
// micrometer calibration landing this close to it is not a real possibility.
constexpr double kNominalTolerance = 1e-5;

bool sameAdapter(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

// k when `m` records sensor pitch / (objective x adapter x k), otherwise 0.
int nominalUpscale(const ImageMetadata &m, double adapter, double sensorPixelUm)
{
    if (m.magnification <= 0 || m.umPerPixel <= 0 || adapter <= 0 || sensorPixelUm <= 0)
        return 0;
    const double base = sensorPixelUm / (m.magnification * adapter);
    for (int k = 1; k <= kMaxUpscale; ++k)
        if (std::abs(m.umPerPixel * k - base) <= kNominalTolerance * base)
            return k;
    return 0;
}

void correct(ImageMetadata &m, const CalibrationFix &fix, double rightAdapter)
{
    m.umPerPixel = fix.newUmPerPixel;
    m.adapterFactor = rightAdapter;
    m.pixelSizeSource = QString::fromLatin1(kPixelSizeNominal); // it was selected as the nominal scale
}

// Writes `m`, corrected, as the image's sidecar.
bool writeCorrectedSidecar(ImageMetadata m, const CalibrationFix &fix, double rightAdapter, QString *error)
{
    correct(m, fix, rightAdapter);
    QSaveFile out(sidecarPath(fix.path));
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = out.errorString();
        return false;
    }
    out.write(QJsonDocument(m.toJson()).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        if (error)
            *error = out.errorString();
        return false;
    }
    return true;
}

// Rewrites the sidecar, if the image has one.
bool fixSidecar(const CalibrationFix &fix, double rightAdapter, QString *error)
{
    ImageMetadata m;
    if (!loadSidecarMetadata(fix.path, m))
        return true; // no sidecar is not a failure
    return writeCorrectedSidecar(m, fix, rightAdapter, error);
}

} // namespace

QList<CalibrationFix> findCalibrationFixes(const QString &folder, bool recursive, double wrongAdapter,
                                           double rightAdapter, QList<CalibrationFix> *leftAlone,
                                           double sensorPixelUm)
{
    QList<CalibrationFix> out;
    if (leftAlone)
        leftAlone->clear();
    if (folder.isEmpty() || rightAdapter <= 0 || wrongAdapter <= 0 || sameAdapter(wrongAdapter, rightAdapter))
        return out;

    QDirIterator it(folder, kImagePatterns, QDir::Files,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        const QString path = it.next();
        const FileFormat fmt = formatFromExtension(path);
        ImageMetadata sidecar, embedded;
        const bool haveSidecar = loadSidecarMetadata(path, sidecar);
        const bool haveEmbedded = loadEmbeddedMetadata(path, embedded);

        // What this program reads (the sidecar wins), and for the formats that
        // are rewritten, also what the file itself says: a repair interrupted
        // between the file and its sidecar leaves them disagreeing, and either
        // one still at the wrong adapter needs the fix. A JPEG's own metadata is
        // never rewritten, so only its sidecar counts once it has one.
        QList<const ImageMetadata *> sources;
        if (haveSidecar)
            sources << &sidecar;
        if (haveEmbedded && (!haveSidecar || fmt != FileFormat::Jpeg))
            sources << &embedded;
        const ImageMetadata *m = nullptr;
        for (const ImageMetadata *s : sources)
            if (s->umPerPixel > 0 && sameAdapter(s->adapterFactor, wrongAdapter)) {
                m = s;
                break;
            }
        if (!m)
            continue;

        CalibrationFix f;
        f.path = path;
        f.oldUmPerPixel = m->umPerPixel;
        f.newUmPerPixel = m->umPerPixel;
        f.recordedAdapter = m->adapterFactor;
        f.rewritesFile = fmt != FileFormat::Jpeg;
        f.createsSidecar = fmt == FileFormat::Jpeg && !haveSidecar;

        // a calibrated or hand-entered scale was right whatever adapter is recorded
        QString source;
        for (const ImageMetadata *s : sources)
            if (source.isEmpty())
                source = s->pixelSizeSource;
        const int k = nominalUpscale(*m, wrongAdapter, sensorPixelUm);
        const bool labelledNominal = source == QLatin1String(kPixelSizeNominal);
        if (source == QLatin1String(kPixelSizeCalibrated))
            f.note = QObject::tr("calibrated objective — left alone");
        else if (source == QLatin1String(kPixelSizeManual))
            f.note = QObject::tr("pixel size set by hand — left alone");
        // (labelled nominal but at no whole upscale of it: a resized export,
        // corrected by the adapter ratio below)
        else if (!labelledNominal && m->magnification <= 0)
            f.note = QObject::tr("no objective magnification recorded — left alone");
        else if (!labelledNominal && k == 0)
            f.note = QObject::tr("calibrated — left alone (not the nominal pixel size for %1×)")
                         .arg(m->magnification, 0, 'g', 4);

        if (!f.note.isEmpty()) {
            if (leftAlone)
                leftAlone->push_back(f);
            continue;
        }
        // The nominal size at the right adapter: absolute, so a second
        // application writes the same value again. A scale labelled nominal
        // but at no whole upscale of it was resized after capture; it is
        // corrected by the adapter ratio (the file then records the right
        // adapter, so it is not selected again).
        f.newUmPerPixel = k > 0 ? sensorPixelUm / (m->magnification * rightAdapter * k)
                                : m->umPerPixel * wrongAdapter / rightAdapter;
        out.push_back(f);
    }
    return out;
}

bool applyCalibrationFix(const CalibrationFix &fix, double rightAdapter, QString *error)
{
    if (!fix.note.isEmpty() || fix.newUmPerPixel <= 0 || rightAdapter <= 0) {
        if (error)
            *error = QObject::tr("this image is not one to correct");
        return false;
    }
    const FileFormat fmt = formatFromExtension(fix.path);

    if (fmt == FileFormat::Tiff) {
        // Read the image back and write it again with the corrected scale. Our
        // own encoder wrote it, so nothing is lost, and writeTiff replaces the
        // file only once the new one is complete.
        Image16 img;
        int bits = 8;
        QString description;
        double umPerPixel = 0;
        if (!readTiff(fix.path, img, bits, description, umPerPixel, error))
            return false;
        ImageMetadata m;
        bool haveMeta = !description.isEmpty() && ImageMetadata::fromJsonString(description, m);
        if (!haveMeta)
            haveMeta = loadSidecarMetadata(fix.path, m);
        correct(m, fix, rightAdapter);
        m.width = img.width;
        m.height = img.height;
        if (!haveMeta)
            m.bitDepth = bits;
        if (!writeTiff(fix.path, img, bits, true, m.umPerPixel, m.toJsonString(), error))
            return false;
        return fixSidecar(fix, rightAdapter, error);
    }

    if (fmt == FileFormat::Png || fmt == FileFormat::Bmp) {
        // Lossless formats: re-encode with the corrected pixel density, and in a
        // PNG the corrected metadata text, which this program reads before the
        // density (a stale one would keep the old scale on screen).
        QImage img;
        {
            // the reader keeps the file open while it exists, and Windows refuses
            // to replace an open file: close it before writing the new one
            QImageReader r(fix.path);
            img = r.read();
            if (img.isNull()) {
                if (error)
                    *error = r.errorString();
                return false;
            }
        }
        const int dpm = int(std::lround(1e6 / fix.newUmPerPixel));
        img.setDotsPerMeterX(dpm);
        img.setDotsPerMeterY(dpm);
        ImageMetadata m;
        if (ImageMetadata::fromJsonString(img.text(QStringLiteral("Description")), m)) {
            correct(m, fix, rightAdapter);
            img.setText(QStringLiteral("Description"), m.toJsonString()); // the writer writes the image's text
        }
        QSaveFile out(fix.path);
        if (!out.open(QIODevice::WriteOnly)) {
            if (error)
                *error = out.errorString();
            return false;
        }
        QImageWriter w(&out, extensionFor(fmt).toLatin1());
        if (fmt == FileFormat::Png)
            w.setCompression(6); // as saveImage writes it
        if (!w.write(img)) {
            if (error)
                *error = w.errorString();
            out.cancelWriting();
            return false;
        }
        if (!out.commit()) {
            if (error)
                *error = out.errorString();
            return false;
        }
        return fixSidecar(fix, rightAdapter, error);
    }

    // JPEG: re-encoding to change the density would cost image quality, so only
    // the sidecar is corrected, and one is created from the file's own metadata
    // when there is none. This program reads the sidecar's pixel size before the
    // file's; a reader that only looks at the JFIF density still sees the old one.
    ImageMetadata m;
    if (!loadMetadata(fix.path, m)) {
        if (error)
            *error = QObject::tr("the image has no metadata to correct");
        return false;
    }
    return writeCorrectedSidecar(m, fix, rightAdapter, error);
}

} // namespace lm
