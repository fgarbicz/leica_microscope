#include "AppSettings.h"

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
            {"grayscale", c.grayscale},   {"invert", c.invert}};
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
    return c;
}

QMap<QString, ColorSettings> AppSettings::builtinPresets()
{
    QMap<QString, ColorSettings> p;
    ColorSettings neutral;
    neutral.blackLevel = 0.002;
    p.insert(QObject::tr("Neutral (linear sRGB)"), neutral);

    ColorSettings ihc = neutral; // DAB/haematoxylin: slightly more contrast and saturation
    ihc.saturation = 1.25;
    ihc.contrast = 1.08;
    ihc.gamma = 1.0;
    ihc.sharpenAmount = 0.35;
    ihc.sharpenRadius = 1.0;
    p.insert(QObject::tr("Bright field - IHC (DAB)"), ihc);

    ColorSettings he = neutral;
    he.saturation = 1.35;
    he.contrast = 1.1;
    he.sharpenAmount = 0.35;
    p.insert(QObject::tr("Bright field - H&E"), he);

    ColorSettings gray = neutral;
    gray.grayscale = true;
    gray.contrast = 1.1;
    p.insert(QObject::tr("Monochrome"), gray);

    ColorSettings pub = neutral; // publication: gentle, no sharpening artefacts
    pub.saturation = 1.1;
    pub.contrast = 1.03;
    p.insert(QObject::tr("Publication (natural)"), pub);
    return p;
}

void AppSettings::load()
{
    QSettings s;
    s.beginGroup(QStringLiteral("capture"));
    capture.folder = s.value(QStringLiteral("folder"), defaultFolder()).toString();
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
    capture.askForNotes = s.value(QStringLiteral("askNotes"), false).toBool();
    capture.openInProcess = s.value(QStringLiteral("openInProcess"), false).toBool();
    capture.sample = s.value(QStringLiteral("sample"), capture.sample).toString();
    capture.operatorName = s.value(QStringLiteral("operator")).toString();
    capture.timelapseIntervalS = s.value(QStringLiteral("tlInterval"), 60.0).toDouble();
    capture.timelapseCount = s.value(QStringLiteral("tlCount"), 10).toInt();
    s.endGroup();

    s.beginGroup(QStringLiteral("overlays"));
    overlays.scaleBar = s.value(QStringLiteral("scaleBar"), true).toBool();
    overlays.grid = s.value(QStringLiteral("grid"), false).toBool();
    overlays.gridDivisions = s.value(QStringLiteral("gridDiv"), 4).toInt();
    overlays.crosshair = s.value(QStringLiteral("crosshair"), false).toBool();
    overlays.clipping = s.value(QStringLiteral("clipping"), false).toBool();
    overlays.focusAssist = s.value(QStringLiteral("focusAssist"), false).toBool();
    overlays.scaleBarPosition = s.value(QStringLiteral("sbPos"), 3).toInt();
    overlays.scaleBarColor = s.value(QStringLiteral("sbColor"), QColor(Qt::white)).value<QColor>();
    overlays.scaleBarBackground = s.value(QStringLiteral("sbBg"), true).toBool();
    overlays.scaleBarLengthUm = s.value(QStringLiteral("sbLen"), 0.0).toDouble();
    s.endGroup();

    s.beginGroup(QStringLiteral("camera"));
    autoExposure = s.value(QStringLiteral("autoExposure"), false).toBool();
    aeTarget = s.value(QStringLiteral("aeTarget"), 0.85).toDouble();
    exposureMs = s.value(QStringLiteral("exposure"), 20.0).toDouble();
    gain = s.value(QStringLiteral("gain"), 1.0).toDouble();
    resolutionIndex = s.value(QStringLiteral("resolution"), 0).toInt();
    lastCameraId = s.value(QStringLiteral("last")).toString();
    shadingEnabled = s.value(QStringLiteral("shading"), false).toBool();
    s.endGroup();

    const QVariant c = s.value(QStringLiteral("color"));
    color = c.isValid() ? colorFromVariant(c.toMap()) : builtinPresets().value(QObject::tr("Bright field - IHC (DAB)"));

    colorPresets.clear();
    s.beginGroup(QStringLiteral("colorPresets"));
    for (const QString &k : s.childKeys())
        colorPresets.insert(k, colorFromVariant(s.value(k).toMap()));
    s.endGroup();

    browseFolder = s.value(QStringLiteral("browseFolder"), capture.folder).toString();
    theme = s.value(QStringLiteral("theme"), theme).toString();
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
    s.setValue(QStringLiteral("askNotes"), capture.askForNotes);
    s.setValue(QStringLiteral("openInProcess"), capture.openInProcess);
    s.setValue(QStringLiteral("sample"), capture.sample);
    s.setValue(QStringLiteral("operator"), capture.operatorName);
    s.setValue(QStringLiteral("tlInterval"), capture.timelapseIntervalS);
    s.setValue(QStringLiteral("tlCount"), capture.timelapseCount);
    s.endGroup();

    s.beginGroup(QStringLiteral("overlays"));
    s.setValue(QStringLiteral("scaleBar"), overlays.scaleBar);
    s.setValue(QStringLiteral("grid"), overlays.grid);
    s.setValue(QStringLiteral("gridDiv"), overlays.gridDivisions);
    s.setValue(QStringLiteral("crosshair"), overlays.crosshair);
    s.setValue(QStringLiteral("clipping"), overlays.clipping);
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
    s.remove(QStringLiteral("colorPresets"));
    s.beginGroup(QStringLiteral("colorPresets"));
    for (auto it = colorPresets.begin(); it != colorPresets.end(); ++it)
        s.setValue(it.key(), colorToVariant(it.value()));
    s.endGroup();
    s.setValue(QStringLiteral("browseFolder"), browseFolder);
    s.setValue(QStringLiteral("theme"), theme);
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
