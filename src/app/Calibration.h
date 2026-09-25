#pragma once
// Microscope configuration: objectives and their spatial calibration.
// µm/px = sensor pixel size / (objective magnification × camera adapter)
// unless a measured calibration (stage micrometer) is stored.

#include <QList>
#include <QString>

namespace lm {

struct Objective {
    QString id;          // stable identifier (file names of per-objective data)
    QString name;
    double magnification = 10.0;
    double na = 0.25;
    QString immersion = QStringLiteral("Dry");
    double calibratedUmPerPixel = 0.0; // 0 = use nominal
    QString shadingFile;               // flat-field reference for this objective
    // camera settings last used with this objective (exposureMs 0 = none stored)
    double exposureMs = 0.0, gain = 1.0;
    double wbRed = 0.0, wbGreen = 0.0, wbBlue = 0.0;
};

class MicroscopeConfig {
public:
    MicroscopeConfig();

    QList<Objective> objectives;
    int current = 4; // 40x
    double adapterFactor = 0.7;   // C-mount adapter magnification
    double sensorPixelUm = 5.86;  // camera pixel pitch
    QString microscopeName = QStringLiteral("Leica DM2000");
    bool rememberSettings = true; // restore exposure/gain/WB when the objective changes

    const Objective &currentObjective() const;
    Objective &currentObjective();
    // µm per pixel for the current objective at the given binning/upscale
    double umPerPixel(double sensorPixelScale = 1.0) const;
    double nominalUmPerPixel(const Objective &o) const;
    QString objectiveLabel(const Objective &o) const;
    // Optical resolution limit (Rayleigh, 550 nm) of the current objective
    double resolutionLimitUm() const;

    void load();
    void save() const;
    static QList<Objective> defaultObjectives();
};

} // namespace lm
