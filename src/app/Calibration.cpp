#include "Calibration.h"

#include <cmath>

#include <QSettings>
#include <QUuid>

#include <algorithm>

namespace lm {

MicroscopeConfig::MicroscopeConfig() : objectives(defaultObjectives()) {}

QList<Objective> MicroscopeConfig::defaultObjectives()
{
    // The objectives on this Leica DM2000 (N PLAN series)
    auto o = [](const char *name, double mag, double na, const char *imm = "Dry") {
        Objective ob;
        ob.name = QString::fromLatin1(name);
        ob.magnification = mag;
        ob.na = na;
        ob.immersion = QString::fromLatin1(imm);
        return ob;
    };
    return {o("N PLAN 2.5x/0.07", 2.5, 0.07), o("N PLAN 5x/0.12", 5, 0.12),   o("N PLAN 10x/0.25", 10, 0.25),
            o("N PLAN 20x/0.40", 20, 0.40),   o("N PLAN 40x/0.65", 40, 0.65), o("N PLAN 100x/1.25 Oil", 100, 1.25, "Oil")};
}

const Objective &MicroscopeConfig::currentObjective() const
{
    return objectives[std::clamp(current, 0, int(objectives.size()) - 1)];
}

Objective &MicroscopeConfig::currentObjective()
{
    return objectives[std::clamp(current, 0, int(objectives.size()) - 1)];
}

double MicroscopeConfig::nominalUmPerPixel(const Objective &o) const
{
    return sensorPixelUm / std::max(1e-6, o.magnification * adapterFactor);
}

double MicroscopeConfig::umPerPixel(double sensorPixelScale) const
{
    if (objectives.isEmpty())
        return 0.0;
    const Objective &o = currentObjective();
    const double base = o.calibratedUmPerPixel > 0 ? o.calibratedUmPerPixel : nominalUmPerPixel(o);
    return base * sensorPixelScale;
}

QString MicroscopeConfig::objectiveLabel(const Objective &o) const
{
    return o.name.isEmpty() ? QStringLiteral("%1x / %2").arg(o.magnification).arg(o.na) : o.name;
}

double MicroscopeConfig::resolutionLimitUm() const
{
    if (objectives.isEmpty())
        return 0.0;
    return 0.61 * 0.55 / std::max(0.01, currentObjective().na);
}

void MicroscopeConfig::load()
{
    QSettings s;
    s.beginGroup(QStringLiteral("microscope"));
    // version 2: objective set of this microscope (2.5/5/10/20/40/100x)
    if (s.value(QStringLiteral("objectivesVersion"), 1).toInt() < 2) {
        s.remove(QStringLiteral("objectives"));
        s.setValue(QStringLiteral("objectivesVersion"), 2);
        s.setValue(QStringLiteral("current"), 4); // 40x
    }
    microscopeName = s.value(QStringLiteral("name"), microscopeName).toString();
    adapterFactor = s.value(QStringLiteral("adapter"), adapterFactor).toDouble();
    // version 3: the camera adapter was assumed to be 0.7x and is really 1.0x.
    // Only the old assumed value is replaced; a figure the user entered or
    // measured is left alone.
    if (s.value(QStringLiteral("objectivesVersion"), 1).toInt() < 3) {
        if (std::abs(adapterFactor - kAssumedAdapterBefore) < 1e-6)
            adapterFactor = 1.0;
        s.setValue(QStringLiteral("objectivesVersion"), 3);
        s.setValue(QStringLiteral("adapter"), adapterFactor);
    }
    sensorPixelUm = s.value(QStringLiteral("sensorPixel"), sensorPixelUm).toDouble();
    current = s.value(QStringLiteral("current"), current).toInt();
    rememberSettings = s.value(QStringLiteral("rememberSettings"), true).toBool();
    const int n = s.beginReadArray(QStringLiteral("objectives"));
    if (n > 0) {
        objectives.clear();
        for (int i = 0; i < n; ++i) {
            s.setArrayIndex(i);
            Objective o;
            o.name = s.value(QStringLiteral("name")).toString();
            o.magnification = s.value(QStringLiteral("mag"), 10.0).toDouble();
            o.na = s.value(QStringLiteral("na"), 0.25).toDouble();
            o.immersion = s.value(QStringLiteral("immersion"), QStringLiteral("Dry")).toString();
            o.calibratedUmPerPixel = s.value(QStringLiteral("umpp"), 0.0).toDouble();
            o.id = s.value(QStringLiteral("id")).toString();
            o.shadingFile = s.value(QStringLiteral("shading")).toString();
            o.exposureMs = s.value(QStringLiteral("exposure"), 0.0).toDouble();
            o.gain = s.value(QStringLiteral("gain"), 1.0).toDouble();
            o.wbRed = s.value(QStringLiteral("wbR"), 0.0).toDouble();
            o.wbGreen = s.value(QStringLiteral("wbG"), 0.0).toDouble();
            o.wbBlue = s.value(QStringLiteral("wbB"), 0.0).toDouble();
            objectives.append(o);
        }
    }
    s.endArray();
    s.endGroup();
    if (objectives.isEmpty())
        objectives = defaultObjectives();
    for (auto &o : objectives)
        if (o.id.isEmpty())
            o.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    current = std::clamp(current, 0, int(objectives.size()) - 1);
}

void MicroscopeConfig::save() const
{
    QSettings s;
    s.beginGroup(QStringLiteral("microscope"));
    s.setValue(QStringLiteral("name"), microscopeName);
    s.setValue(QStringLiteral("adapter"), adapterFactor);
    s.setValue(QStringLiteral("sensorPixel"), sensorPixelUm);
    s.setValue(QStringLiteral("current"), current);
    s.setValue(QStringLiteral("rememberSettings"), rememberSettings);
    s.beginWriteArray(QStringLiteral("objectives"), int(objectives.size()));
    for (int i = 0; i < objectives.size(); ++i) {
        s.setArrayIndex(i);
        const Objective &o = objectives[i];
        s.setValue(QStringLiteral("name"), o.name);
        s.setValue(QStringLiteral("mag"), o.magnification);
        s.setValue(QStringLiteral("na"), o.na);
        s.setValue(QStringLiteral("immersion"), o.immersion);
        s.setValue(QStringLiteral("umpp"), o.calibratedUmPerPixel);
        s.setValue(QStringLiteral("id"), o.id);
        s.setValue(QStringLiteral("shading"), o.shadingFile);
        s.setValue(QStringLiteral("exposure"), o.exposureMs);
        s.setValue(QStringLiteral("gain"), o.gain);
        s.setValue(QStringLiteral("wbR"), o.wbRed);
        s.setValue(QStringLiteral("wbG"), o.wbGreen);
        s.setValue(QStringLiteral("wbB"), o.wbBlue);
    }
    s.endArray();
    s.endGroup();
}

} // namespace lm
