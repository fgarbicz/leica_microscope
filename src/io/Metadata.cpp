#include "Metadata.h"

#include <QJsonDocument>

namespace lm {

QJsonObject ImageMetadata::toJson() const
{
    QJsonObject o;
    o["software"] = software;
    o["softwareVersion"] = softwareVersion;
    o["acquired"] = acquired.toString(Qt::ISODateWithMs);
    o["camera"] = camera;
    o["cameraSerial"] = cameraSerial;
    o["sensor"] = sensor;
    o["microscope"] = microscope;
    o["objective"] = objective;
    o["magnification"] = magnification;
    o["numericalAperture"] = numericalAperture;
    o["adapterFactor"] = adapterFactor;
    o["umPerPixel"] = umPerPixel;
    o["exposureMs"] = exposureMs;
    o["gain"] = gain;
    o["bitDepth"] = bitDepth;
    o["averagedFrames"] = averagedFrames;
    o["captureMode"] = captureMode;
    o["width"] = width;
    o["height"] = height;
    o["whiteBalance"] = QJsonObject{{"r", wbRed}, {"g", wbGreen}, {"b", wbBlue}};
    o["gamma"] = gamma;
    o["saturation"] = saturation;
    o["blackLevel"] = blackLevel;
    o["shadingCorrected"] = shadingCorrected;
    o["sample"] = sample;
    o["operator"] = operatorName;
    o["notes"] = notes;
    return o;
}

ImageMetadata ImageMetadata::fromJson(const QJsonObject &o)
{
    ImageMetadata m;
    m.software = o["software"].toString(m.software);
    m.softwareVersion = o["softwareVersion"].toString();
    m.acquired = QDateTime::fromString(o["acquired"].toString(), Qt::ISODateWithMs);
    m.camera = o["camera"].toString();
    m.cameraSerial = o["cameraSerial"].toString();
    m.sensor = o["sensor"].toString();
    m.microscope = o["microscope"].toString(m.microscope);
    m.objective = o["objective"].toString();
    m.magnification = o["magnification"].toDouble();
    m.numericalAperture = o["numericalAperture"].toDouble();
    m.adapterFactor = o["adapterFactor"].toDouble(1.0);
    m.umPerPixel = o["umPerPixel"].toDouble();
    m.exposureMs = o["exposureMs"].toDouble();
    m.gain = o["gain"].toDouble(1.0);
    m.bitDepth = o["bitDepth"].toInt(8);
    m.averagedFrames = o["averagedFrames"].toInt(1);
    m.captureMode = o["captureMode"].toString();
    m.width = o["width"].toInt();
    m.height = o["height"].toInt();
    const QJsonObject wb = o["whiteBalance"].toObject();
    m.wbRed = wb["r"].toDouble(1);
    m.wbGreen = wb["g"].toDouble(1);
    m.wbBlue = wb["b"].toDouble(1);
    m.gamma = o["gamma"].toDouble(1);
    m.saturation = o["saturation"].toDouble(1);
    m.blackLevel = o["blackLevel"].toDouble(0);
    m.shadingCorrected = o["shadingCorrected"].toBool();
    m.sample = o["sample"].toString();
    m.operatorName = o["operator"].toString();
    m.notes = o["notes"].toString();
    return m;
}

QString ImageMetadata::toJsonString() const
{
    return QString::fromUtf8(QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}

bool ImageMetadata::fromJsonString(const QString &s, ImageMetadata &out)
{
    QJsonParseError err{};
    const QJsonDocument d = QJsonDocument::fromJson(s.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !d.isObject())
        return false;
    out = fromJson(d.object());
    return true;
}

QList<QPair<QString, QString>> ImageMetadata::describe() const
{
    QList<QPair<QString, QString>> l;
    auto add = [&](const QString &k, const QString &v) {
        if (!v.isEmpty())
            l.append({k, v});
    };
    add(QObject::tr("Acquired"), acquired.isValid() ? QLocale().toString(acquired, QLocale::LongFormat) : QString());
    add(QObject::tr("Image size"), width > 0 ? QStringLiteral("%1 × %2 px, %3-bit").arg(width).arg(height).arg(bitDepth) : QString());
    add(QObject::tr("Capture mode"), captureMode);
    add(QObject::tr("Microscope"), microscope);
    add(QObject::tr("Objective"), objective);
    add(QObject::tr("Pixel size"), umPerPixel > 0 ? QStringLiteral("%1 µm/px").arg(umPerPixel, 0, 'g', 5) : QString());
    add(QObject::tr("Field of view"),
        umPerPixel > 0 && width > 0
            ? QStringLiteral("%1 × %2 µm").arg(width * umPerPixel, 0, 'f', 1).arg(height * umPerPixel, 0, 'f', 1)
            : QString());
    add(QObject::tr("Camera"), camera + (cameraSerial.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(cameraSerial)));
    add(QObject::tr("Sensor"), sensor);
    add(QObject::tr("Exposure"), exposureMs > 0 ? QStringLiteral("%1 ms").arg(exposureMs, 0, 'g', 4) : QString());
    add(QObject::tr("Gain"), QStringLiteral("%1×").arg(gain, 0, 'f', 2));
    add(QObject::tr("Frames averaged"), averagedFrames > 1 ? QString::number(averagedFrames) : QString());
    add(QObject::tr("White balance"), QStringLiteral("R %1  G %2  B %3").arg(wbRed, 0, 'f', 3).arg(wbGreen, 0, 'f', 3).arg(wbBlue, 0, 'f', 3));
    add(QObject::tr("Shading correction"), shadingCorrected ? QObject::tr("applied") : QObject::tr("off"));
    add(QObject::tr("Sample"), sample);
    add(QObject::tr("Operator"), operatorName);
    add(QObject::tr("Notes"), notes);
    add(QObject::tr("Software"), software + QLatin1Char(' ') + softwareVersion);
    return l;
}

} // namespace lm
