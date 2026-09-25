#pragma once
// Persistent application settings (QSettings, per user).

#include "imaging/ColorPipeline.h"
#include "io/ImageIO.h"

#include <QColor>
#include <QMap>
#include <QString>
#include <QStringList>

namespace lm {

struct CaptureSettings {
    QString folder;                          // output directory
    QString pattern = QStringLiteral("{sample}_{objective}_{date}_{counter}"); // file name template
    int counter = 1;
    int counterDigits = 3;
    SaveOptions save;
    int averageFrames = 1;                   // frame averaging for noise reduction
    int shotMode = -1;                       // -1 = single shot, else camera shot mode (pixel shift)
    bool burnScaleBar = false;               // render scale bar into exported pixels
    bool burnAnnotations = false;
    bool promptAfterCapture = true;          // ask for magnification + name after each capture
    bool openInProcess = false;              // switch to Process workspace after capture
    QString sample = QStringLiteral("Sample");
    QString operatorName;
    // time lapse
    double timelapseIntervalS = 60.0;
    int timelapseCount = 10;
    // video
    int videoFps = 25;
    bool videoScaleBar = true;
};

// IHC (DAB) quantification: threshold and stain vectors shared by the Process
// page and batch quantification
struct IhcSettings {
    double dabThreshold = 0.15;
    bool customVectors = false;              // false = standard Ruifrok H-DAB vectors
    double h[3] = {0.650, 0.704, 0.286};
    double dab[3] = {0.268, 0.570, 0.776};
    QString vectorSource;                    // e.g. image the vectors were estimated from
    double nucleusDiameterUm = 7.0;          // nucleus counting
    bool nuclearMarker = true;               // marker in nuclei (Ki-67...) or cytoplasm/membrane
    int nucleusSensitivity = 1;              // 0 low, 1 normal, 2 high
};

struct OverlaySettings {
    bool scaleBar = true;
    bool grid = false;
    int gridDivisions = 4;
    bool crosshair = false;
    bool clipping = false;
    bool focusAssist = false;
    bool liveDab = false;                    // live IHC (DAB) overlay
    int scaleBarPosition = 3;                // 0 TL, 1 TR, 2 BL, 3 BR
    QColor scaleBarColor = Qt::white;
    bool scaleBarBackground = true;
    double scaleBarLengthUm = 0.0;           // 0 = automatic
    QColor gridColor = QColor(255, 255, 255, 110);
};

class AppSettings {
public:
    static AppSettings &instance();

    CaptureSettings capture;
    OverlaySettings overlays;
    IhcSettings ihc;
    ColorSettings color;
    bool autoExposure = false;
    double aeTarget = 0.85;
    double exposureMs = 20.0;
    double gain = 1.0;
    int resolutionIndex = 0;
    QString lastCameraId;
    QString browseFolder;
    QString theme = QStringLiteral("dark");
    // Size of the whole interface in percent (text, controls, icons and
    // spacing). 100 is the platform default; the microscope room is often read
    // from a metre away, and some people simply want it bigger.
    int uiScale = 100;
    bool shadingEnabled = false;
    QMap<QString, ColorSettings> colorPresets; // user presets

    void load();
    void save() const;

    // Expands the file name pattern for the next capture (does not increment).
    QString nextFileName(const QString &objective, const QString &mode) const;
    static QString defaultFolder();

    static QMap<QString, ColorSettings> builtinPresets();
};

QVariantMap colorToVariant(const ColorSettings &c);
ColorSettings colorFromVariant(const QVariantMap &m);

} // namespace lm
