#pragma once
// Acquisition metadata stored with every image (TIFF ImageDescription as JSON,
// and a .json sidecar for formats without text tags).

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace lm {

// Where ImageMetadata::pixelSizeSource says the pixel size came from.
inline constexpr char kPixelSizeNominal[] = "nominal";       // sensor pitch / (objective x adapter)
inline constexpr char kPixelSizeCalibrated[] = "calibrated"; // the objective's calibrated value (stage micrometer)
inline constexpr char kPixelSizeManual[] = "manual";         // entered by hand

struct ImageMetadata {
    QString software = QStringLiteral("DM Imaging");
    QString softwareVersion;
    QDateTime acquired;
    QString camera;
    QString cameraSerial;
    QString sensor;
    QString microscope = QStringLiteral("Leica DM2000");
    QString objective;           // e.g. "HC PL FLUOTAR 20x/0.50"
    double magnification = 0.0;  // objective magnification
    double numericalAperture = 0.0;
    double adapterFactor = 1.0;  // C-mount / camera adapter magnification
    double umPerPixel = 0.0;     // calibrated pixel size in the specimen plane
    QString pixelSizeSource;     // kPixelSizeNominal / Calibrated / Manual; empty = not recorded (older files)
    double exposureMs = 0.0;
    QVector<double> exposureSeriesMs; // HDR: every exposure merged (exposureMs = the reference)
    double gain = 1.0;
    int bitDepth = 8;
    int averagedFrames = 1;
    QString captureMode;         // single, multifocus, mosaic, pixelshift-4, ...
    int width = 0, height = 0;
    double wbRed = 1, wbGreen = 1, wbBlue = 1;
    double gamma = 1, saturation = 1, blackLevel = 0;
    bool shadingCorrected = false;
    QString lightFilter;          // e.g. "temperature +40, tint 0"; empty = none
    QString colorCorrection;      // e.g. "camera matrix", "off"; empty = not recorded (older files)
    QString sample;              // user fields
    QString operatorName;
    QString notes;

    QJsonObject toJson() const;
    static ImageMetadata fromJson(const QJsonObject &o);
    QString toJsonString() const;
    static bool fromJsonString(const QString &s, ImageMetadata &out);
    // Human readable key/value list for the info panel.
    QList<QPair<QString, QString>> describe() const;
};

} // namespace lm
