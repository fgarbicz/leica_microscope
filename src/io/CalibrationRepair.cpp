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

bool sameAdapter(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

QString sidecarFor(const QString &path)
{
    return path + QStringLiteral(".json");
}

// Rewrites the sidecar, if the image has one.
bool fixSidecar(const QString &path, double newUmPerPixel, double rightAdapter, QString *error)
{
    const QString sc = sidecarFor(path);
    QFile in(sc);
    if (!in.open(QIODevice::ReadOnly))
        return true; // no sidecar is not a failure
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll());
    in.close();
    if (!doc.isObject())
        return true;
    ImageMetadata m = ImageMetadata::fromJson(doc.object());
    m.umPerPixel = newUmPerPixel;
    m.adapterFactor = rightAdapter;
    QSaveFile out(sc);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = out.errorString();
        return false;
    }
    out.write(m.toJsonString().toUtf8());
    if (!out.commit()) {
        if (error)
            *error = out.errorString();
        return false;
    }
    return true;
}

} // namespace

QList<CalibrationFix> findCalibrationFixes(const QString &folder, bool recursive, double wrongAdapter,
                                           double rightAdapter)
{
    QList<CalibrationFix> out;
    if (folder.isEmpty() || rightAdapter <= 0 || wrongAdapter <= 0 || sameAdapter(wrongAdapter, rightAdapter))
        return out;
    const double scale = wrongAdapter / rightAdapter;

    QDirIterator it(folder, kImagePatterns, QDir::Files,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        const QString path = it.next();
        ImageMetadata m;
        if (!loadMetadata(path, m))
            continue;
        if (m.umPerPixel <= 0 || !sameAdapter(m.adapterFactor, wrongAdapter))
            continue;
        CalibrationFix f;
        f.path = path;
        f.oldUmPerPixel = m.umPerPixel;
        f.newUmPerPixel = m.umPerPixel * scale;
        f.recordedAdapter = m.adapterFactor;
        const FileFormat fmt = formatFromExtension(path);
        f.rewritesFile = fmt != FileFormat::Jpeg;
        out.push_back(f);
    }
    return out;
}

bool applyCalibrationFix(const CalibrationFix &fix, double rightAdapter, QString *error)
{
    const FileFormat fmt = formatFromExtension(fix.path);

    if (fmt == FileFormat::Tiff) {
        // Read the image back and write it again with the corrected scale. Our
        // own encoder wrote it, so nothing is lost; the temporary file becomes
        // the original only once it is complete.
        Image16 img;
        int bits = 8;
        QString description;
        double umPerPixel = 0;
        if (!readTiff(fix.path, img, bits, description, umPerPixel, error))
            return false;
        ImageMetadata m;
        bool haveMeta = false;
        if (!description.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(description.toUtf8());
            if (doc.isObject()) {
                m = ImageMetadata::fromJson(doc.object());
                haveMeta = true;
            }
        }
        m.umPerPixel = fix.newUmPerPixel;
        m.adapterFactor = rightAdapter;
        m.width = img.width;
        m.height = img.height;
        if (!haveMeta)
            m.bitDepth = bits;

        const QString tmp = fix.path + QStringLiteral(".fixing");
        QFile::remove(tmp);
        if (!writeTiff(tmp, img, bits, true, m.umPerPixel, m.toJsonString(), error)) {
            QFile::remove(tmp);
            return false;
        }
        // replace the original only now that the new file is complete
        const QString backup = fix.path + QStringLiteral(".replacing");
        QFile::remove(backup);
        if (!QFile::rename(fix.path, backup)) {
            QFile::remove(tmp);
            if (error)
                *error = QObject::tr("cannot replace the file");
            return false;
        }
        if (!QFile::rename(tmp, fix.path)) {
            QFile::rename(backup, fix.path); // put the original back
            QFile::remove(tmp);
            if (error)
                *error = QObject::tr("cannot put the corrected file in place");
            return false;
        }
        QFile::remove(backup);
        return fixSidecar(fix.path, fix.newUmPerPixel, rightAdapter, error);
    }

    if (fmt == FileFormat::Png || fmt == FileFormat::Bmp) {
        // Lossless formats: re-encode with the corrected pixel density.
        QImageReader r(fix.path);
        r.setAllocationLimit(4096);
        QImage img = r.read();
        if (img.isNull()) {
            if (error)
                *error = r.errorString();
            return false;
        }
        const int dpm = int(std::lround(1e6 / fix.newUmPerPixel));
        img.setDotsPerMeterX(dpm);
        img.setDotsPerMeterY(dpm);
        QSaveFile out(fix.path);
        if (!out.open(QIODevice::WriteOnly)) {
            if (error)
                *error = out.errorString();
            return false;
        }
        QImageWriter w(&out, extensionFor(fmt).toLatin1());
        if (!w.write(img)) {
            if (error)
                *error = w.errorString();
            return false;
        }
        if (!out.commit()) {
            if (error)
                *error = out.errorString();
            return false;
        }
        return fixSidecar(fix.path, fix.newUmPerPixel, rightAdapter, error);
    }

    // JPEG: re-encoding to change the density would cost image quality, so only
    // the sidecar is corrected. This program and the sidecar agree; a reader
    // that only looks at the JFIF density will still see the old value.
    return fixSidecar(fix.path, fix.newUmPerPixel, rightAdapter, error);
}

} // namespace lm
