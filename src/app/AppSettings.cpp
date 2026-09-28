#include "AppSettings.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace lm {

AppSettings &AppSettings::instance()
{
    static AppSettings s;
    return s;
}

QString AppSettings::defaultFolder()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).filePath(QStringLiteral("DM Imaging"));
}

QVariantMap colorToVariant(const ColorSettings &c)
{
    return {{"blackLevel", c.blackLevel}, {"wbRed", c.wbRed},         {"wbGreen", c.wbGreen},
            {"wbBlue", c.wbBlue},         {"saturation", c.saturation}, {"hue", c.hue},
            {"blackPoint", c.blackPoint}, {"whitePoint", c.whitePoint}, {"brightness", c.brightness},
            {"contrast", c.contrast},     {"gamma", c.gamma},           {"srgb", c.srgbEncode},
            {"sharpen", c.sharpenAmount}, {"sharpenRadius", c.sharpenRadius},
            {"flipH", c.flipHorizontal},  {"flipV", c.flipVertical},    {"rotation", c.rotation},
            {"grayscale", c.grayscale},   {"invert", c.invert},
            {"colorCorrection", c.colorCorrection},
            {"filterTemperature", c.filterTemperature}, {"filterTint", c.filterTint}};
}

ColorSettings colorFromVariant(const QVariantMap &m)
{
    ColorSettings c;
    auto d = [&](const char *k, double def) { return m.value(QLatin1String(k), def).toDouble(); };
    auto b = [&](const char *k, bool def) { return m.value(QLatin1String(k), def).toBool(); };
    c.blackLevel = d("blackLevel", c.blackLevel);
    c.wbRed = d("wbRed", 1);
    c.wbGreen = d("wbGreen", 1);
    c.wbBlue = d("wbBlue", 1);
    c.saturation = d("saturation", 1);
    c.hue = d("hue", 0);
    c.blackPoint = d("blackPoint", 0);
    c.whitePoint = d("whitePoint", 1);
    c.brightness = d("brightness", 0);
    c.contrast = d("contrast", 1);
    c.gamma = d("gamma", 1);
    c.srgbEncode = b("srgb", true);
    c.sharpenAmount = d("sharpen", 0);
    c.sharpenRadius = d("sharpenRadius", 1);
    c.flipHorizontal = b("flipH", false);
    c.flipVertical = b("flipV", false);
    c.rotation = m.value(QStringLiteral("rotation"), 0).toInt();
    c.grayscale = b("grayscale", false);
    c.invert = b("invert", false);
    c.colorCorrection = b("colorCorrection", true);
    c.filterTemperature = std::clamp(d("filterTemperature", 0), -100.0, 100.0);
    c.filterTint = std::clamp(d("filterTint", 0), -100.0, 100.0);
    return c;
}

QMap<QString, ColorSettings> AppSettings::builtinPresets()
{
    QMap<QString, ColorSettings> p;
    ColorSettings neutral;
    neutral.blackLevel = 0.002;
    p.insert(QObject::tr("Neutral (linear sRGB)"), neutral);

    // saturation: the camera colour matrix already gives calibrated colours
    ColorSettings ihc = neutral; // DAB/haematoxylin: slightly more contrast
    ihc.saturation = 1.0;
    ihc.contrast = 1.08;
    ihc.gamma = 1.0;
    ihc.sharpenAmount = 0.35;
    ihc.sharpenRadius = 1.0;
    p.insert(QObject::tr("Bright field - IHC (DAB)"), ihc);

    ColorSettings he = neutral;
    he.saturation = 1.1;
    he.contrast = 1.1;
    he.sharpenAmount = 0.35;
    p.insert(QObject::tr("Bright field - H&E"), he);

    ColorSettings gray = neutral;
    gray.grayscale = true;
    gray.contrast = 1.1;
    p.insert(QObject::tr("Monochrome"), gray);

    ColorSettings pub = neutral; // publication: gentle, no sharpening artefacts
    pub.saturation = 1.0;
    pub.contrast = 1.03;
    p.insert(QObject::tr("Publication (natural)"), pub);
    return p;
}

void AppSettings::load()
{
    QSettings s;
    s.beginGroup(QStringLiteral("capture"));
    capture.folder = s.value(QStringLiteral("folder"), defaultFolder()).toString();
    // an empty or relative folder would put captures in the working directory
    // ("/" for an application started from the Finder)
    if (capture.folder.trimmed().isEmpty() || QDir::isRelativePath(capture.folder))
        capture.folder = defaultFolder();
    capture.pattern = s.value(QStringLiteral("pattern"), capture.pattern).toString();
    capture.counter = s.value(QStringLiteral("counter"), 1).toInt();
    capture.counterDigits = s.value(QStringLiteral("digits"), 3).toInt();
    capture.save.format = FileFormat(s.value(QStringLiteral("format"), int(FileFormat::Tiff)).toInt());
    capture.save.sixteenBit = s.value(QStringLiteral("16bit"), true).toBool();
    capture.save.compress = s.value(QStringLiteral("compress"), true).toBool();
    capture.save.jpegQuality = s.value(QStringLiteral("jpegQuality"), 95).toInt();
    capture.averageFrames = s.value(QStringLiteral("average"), 1).toInt();
    capture.shotMode = s.value(QStringLiteral("shotMode"), -1).toInt();
    capture.burnScaleBar = s.value(QStringLiteral("burnScale"), false).toBool();
    capture.burnAnnotations = s.value(QStringLiteral("burnAnn"), false).toBool();
    capture.promptAfterCapture = s.value(QStringLiteral("promptAfterCapture"), true).toBool();
    capture.openInProcess = s.value(QStringLiteral("openInProcess"), false).toBool();
    capture.sample = s.value(QStringLiteral("sample"), capture.sample).toString();
    capture.operatorName = s.value(QStringLiteral("operator")).toString();
    capture.timelapseIntervalS = s.value(QStringLiteral("tlInterval"), 60.0).toDouble();
    capture.timelapseCount = s.value(QStringLiteral("tlCount"), 10).toInt();
    capture.videoFps = s.value(QStringLiteral("videoFps"), 25).toInt();
    capture.videoScaleBar = s.value(QStringLiteral("videoScaleBar"), true).toBool();
    s.endGroup();

    s.beginGroup(QStringLiteral("ihc"));
    ihc.dabThreshold = s.value(QStringLiteral("threshold"), 0.15).toDouble();
    ihc.customVectors = s.value(QStringLiteral("custom"), false).toBool();
    ihc.vectorSource = s.value(QStringLiteral("source")).toString();
    ihc.nucleusDiameterUm = s.value(QStringLiteral("nucleusDiameter"), 7.0).toDouble();
    ihc.nuclearMarker = s.value(QStringLiteral("nuclearMarker"), true).toBool();
    ihc.nucleusSensitivity = std::clamp(s.value(QStringLiteral("nucleusSensitivity"), 1).toInt(), 0, 2);
    {
        const QVariantList hv = s.value(QStringLiteral("h")).toList(), dv = s.value(QStringLiteral("dab")).toList();
        if (hv.size() == 3 && dv.size() == 3)
            for (int c = 0; c < 3; ++c) {
                ihc.h[c] = hv[c].toDouble();
                ihc.dab[c] = dv[c].toDouble();
            }
        else
            ihc.customVectors = false;
    }
    s.endGroup();

    s.beginGroup(QStringLiteral("overlays"));
    overlays.scaleBar = s.value(QStringLiteral("scaleBar"), true).toBool();
    overlays.grid = s.value(QStringLiteral("grid"), false).toBool();
    overlays.gridDivisions = s.value(QStringLiteral("gridDiv"), 4).toInt();
    overlays.crosshair = s.value(QStringLiteral("crosshair"), false).toBool();
    overlays.clipping = s.value(QStringLiteral("clipping"), false).toBool();
    overlays.liveDab = s.value(QStringLiteral("liveDab"), false).toBool();
    overlays.focusAssist = s.value(QStringLiteral("focusAssist"), false).toBool();
    overlays.scaleBarPosition = s.value(QStringLiteral("sbPos"), 3).toInt();
    overlays.scaleBarColor = s.value(QStringLiteral("sbColor"), QColor(Qt::white)).value<QColor>();
    overlays.scaleBarBackground = s.value(QStringLiteral("sbBg"), true).toBool();
    overlays.scaleBarLengthUm = s.value(QStringLiteral("sbLen"), 0.0).toDouble();
    s.endGroup();

    s.beginGroup(QStringLiteral("camera"));
    autoExposure = s.value(QStringLiteral("autoExposure"), false).toBool();
    aeTarget = s.value(QStringLiteral("aeTarget"), 0.80).toDouble();
    exposureMs = s.value(QStringLiteral("exposure"), 20.0).toDouble();
    gain = s.value(QStringLiteral("gain"), 1.0).toDouble();
    resolutionIndex = s.value(QStringLiteral("resolution"), 0).toInt();
    lastCameraId = s.value(QStringLiteral("last")).toString();
    shadingEnabled = s.value(QStringLiteral("shading"), false).toBool();
    s.endGroup();

    const QVariant c = s.value(QStringLiteral("color"));
    color = c.isValid() ? colorFromVariant(c.toMap()) : builtinPresets().value(QObject::tr("Bright field - IHC (DAB)"));

    // Presets are a list of maps that carry their name as a value: a name used
    // as a QSettings key is split into sub-groups at every '/' ("H&E / DAB"),
    // and such a preset never came back.
    colorPresets.clear();
    const QVariantList presetList = s.value(QStringLiteral("colorPresetList")).toList();
    for (const QVariant &v : presetList) {
        const QVariantMap m = v.toMap();
        const QString name = m.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            colorPresets.insert(name, colorFromVariant(m.value(QStringLiteral("settings")).toMap()));
    }
    if (presetList.isEmpty()) {
        // settings written before the list: one key per preset. allKeys() puts
        // back the '/' of names that were split into sub-groups.
        s.beginGroup(QStringLiteral("colorPresets"));
        for (const QString &k : s.allKeys())
            colorPresets.insert(k, colorFromVariant(s.value(k).toMap()));
        s.endGroup();
    }

    browseFolder = s.value(QStringLiteral("browseFolder"), capture.folder).toString();
    theme = s.value(QStringLiteral("theme"), theme).toString();
    uiScale = std::clamp(s.value(QStringLiteral("uiScale"), uiScale).toInt(), 75, 200);
    galleryVertical = s.value(QStringLiteral("galleryVertical"), galleryVertical).toBool();
    galleryCompact = s.value(QStringLiteral("galleryCompact"), galleryCompact).toBool();
}

void AppSettings::save() const
{
    QSettings s;
    s.beginGroup(QStringLiteral("capture"));
    s.setValue(QStringLiteral("folder"), capture.folder);
    s.setValue(QStringLiteral("pattern"), capture.pattern);
    s.setValue(QStringLiteral("counter"), capture.counter);
    s.setValue(QStringLiteral("digits"), capture.counterDigits);
    s.setValue(QStringLiteral("format"), int(capture.save.format));
    s.setValue(QStringLiteral("16bit"), capture.save.sixteenBit);
    s.setValue(QStringLiteral("compress"), capture.save.compress);
    s.setValue(QStringLiteral("jpegQuality"), capture.save.jpegQuality);
    s.setValue(QStringLiteral("average"), capture.averageFrames);
    s.setValue(QStringLiteral("shotMode"), capture.shotMode);
    s.setValue(QStringLiteral("burnScale"), capture.burnScaleBar);
    s.setValue(QStringLiteral("burnAnn"), capture.burnAnnotations);
    s.setValue(QStringLiteral("promptAfterCapture"), capture.promptAfterCapture);
    s.setValue(QStringLiteral("openInProcess"), capture.openInProcess);
    s.setValue(QStringLiteral("sample"), capture.sample);
    s.setValue(QStringLiteral("operator"), capture.operatorName);
    s.setValue(QStringLiteral("tlInterval"), capture.timelapseIntervalS);
    s.setValue(QStringLiteral("tlCount"), capture.timelapseCount);
    s.setValue(QStringLiteral("videoFps"), capture.videoFps);
    s.setValue(QStringLiteral("videoScaleBar"), capture.videoScaleBar);
    s.endGroup();

    s.beginGroup(QStringLiteral("ihc"));
    s.setValue(QStringLiteral("threshold"), ihc.dabThreshold);
    s.setValue(QStringLiteral("custom"), ihc.customVectors);
    s.setValue(QStringLiteral("source"), ihc.vectorSource);
    s.setValue(QStringLiteral("nucleusDiameter"), ihc.nucleusDiameterUm);
    s.setValue(QStringLiteral("nuclearMarker"), ihc.nuclearMarker);
    s.setValue(QStringLiteral("nucleusSensitivity"), ihc.nucleusSensitivity);
    s.setValue(QStringLiteral("h"), QVariantList{ihc.h[0], ihc.h[1], ihc.h[2]});
    s.setValue(QStringLiteral("dab"), QVariantList{ihc.dab[0], ihc.dab[1], ihc.dab[2]});
    s.endGroup();

    s.beginGroup(QStringLiteral("overlays"));
    s.setValue(QStringLiteral("scaleBar"), overlays.scaleBar);
    s.setValue(QStringLiteral("grid"), overlays.grid);
    s.setValue(QStringLiteral("gridDiv"), overlays.gridDivisions);
    s.setValue(QStringLiteral("crosshair"), overlays.crosshair);
    s.setValue(QStringLiteral("clipping"), overlays.clipping);
    s.setValue(QStringLiteral("liveDab"), overlays.liveDab);
    s.setValue(QStringLiteral("focusAssist"), overlays.focusAssist);
    s.setValue(QStringLiteral("sbPos"), overlays.scaleBarPosition);
    s.setValue(QStringLiteral("sbColor"), overlays.scaleBarColor);
    s.setValue(QStringLiteral("sbBg"), overlays.scaleBarBackground);
    s.setValue(QStringLiteral("sbLen"), overlays.scaleBarLengthUm);
    s.endGroup();

    s.beginGroup(QStringLiteral("camera"));
    s.setValue(QStringLiteral("autoExposure"), autoExposure);
    s.setValue(QStringLiteral("aeTarget"), aeTarget);
    s.setValue(QStringLiteral("exposure"), exposureMs);
    s.setValue(QStringLiteral("gain"), gain);
    s.setValue(QStringLiteral("resolution"), resolutionIndex);
    s.setValue(QStringLiteral("last"), lastCameraId);
    s.setValue(QStringLiteral("shading"), shadingEnabled);
    s.endGroup();

    s.setValue(QStringLiteral("color"), colorToVariant(color));
    s.remove(QStringLiteral("colorPresets")); // the old one-key-per-preset form, see load()
    QVariantList presetList;
    for (auto it = colorPresets.begin(); it != colorPresets.end(); ++it)
        presetList.push_back(QVariantMap{{QStringLiteral("name"), it.key()},
                                         {QStringLiteral("settings"), colorToVariant(it.value())}});
    s.setValue(QStringLiteral("colorPresetList"), presetList);
    s.setValue(QStringLiteral("browseFolder"), browseFolder);
    s.setValue(QStringLiteral("theme"), theme);
    s.setValue(QStringLiteral("uiScale"), uiScale);
    s.setValue(QStringLiteral("galleryVertical"), galleryVertical);
    s.setValue(QStringLiteral("galleryCompact"), galleryCompact);
}

QString AppSettings::nextFileName(const QString &objective, const QString &mode) const
{
    const QDateTime now = QDateTime::currentDateTime();
    QString name = capture.pattern;
    QString obj = objective;
    obj.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9.]+")), QStringLiteral("-"));
    QString sample = capture.sample;
    sample.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    name.replace(QStringLiteral("{sample}"), sample);
    name.replace(QStringLiteral("{objective}"), obj);
    name.replace(QStringLiteral("{date}"), now.toString(QStringLiteral("yyyyMMdd")));
    name.replace(QStringLiteral("{time}"), now.toString(QStringLiteral("HHmmss")));
    name.replace(QStringLiteral("{mode}"), mode);
    name.replace(QStringLiteral("{operator}"), capture.operatorName);
    name.replace(QStringLiteral("{counter}"), QStringLiteral("%1").arg(capture.counter, capture.counterDigits, 10, QLatin1Char('0')));
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    if (name.trimmed().isEmpty())
        name = QStringLiteral("image_%1").arg(capture.counter, capture.counterDigits, 10, QLatin1Char('0'));
    return QDir(capture.folder).filePath(name + QLatin1Char('.') + extensionFor(capture.save.format));
}

} // namespace lm
