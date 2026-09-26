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
    // C-mount adapter magnification. 1.0 is measured, not assumed: LAS X
    // records 1124.83 um across 3840 px at 10x and 281.21 um at 40x on this
    // microscope, which is exactly 5.86 um / (objective x 1.0). It used to
    // default to an assumed 0.7, which made every nominal pixel size 43 % too
    // large; MicroscopeConfig::load() corrects that value where it was stored.
    static constexpr double kAssumedAdapterBefore = 0.7;
    double adapterFactor = 1.0;
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
