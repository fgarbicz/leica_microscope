#include "ColorPanel.h"

#include "app/AppSettings.h"
#include "ui/CollapsibleSection.h"
#include "ui/Icons.h"
#include "ui/PanelGroup.h"
#include "ui/SliderSpin.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPointF>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lm {

ColorPanel::ColorPanel(QWidget *parent) : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Everything that changes how the image looks, in the order it is used:
    // neutralise the white, correct the lamp, set the tone, then the finish.
    auto *group = new PanelGroup(tr("Adjust"), Icon::Palette, theme().groupAdjust, this);
    root->addWidget(group);

    // --- presets
    auto *pre = group->addSection(tr("White balance & presets"), Icon::Palette);
    auto *prow = new QHBoxLayout;
    m_presets = new QComboBox(this);
    auto *savePreset = new QToolButton(this);
    savePreset->setIcon(icon(Icon::Plus, theme().subText, 15));
    savePreset->setToolTip(tr("Save current settings as a preset"));
    auto *delPreset = new QToolButton(this);
    delPreset->setIcon(icon(Icon::Minus, theme().subText, 15));
    delPreset->setToolTip(tr("Delete the selected user preset"));
    prow->addWidget(m_presets, 1);
    prow->addWidget(savePreset);
    prow->addWidget(delPreset);
    pre->contentLayout()->addLayout(prow);

    // white balance
    auto *wbRow = new QHBoxLayout;
    auto *wbAuto = new QPushButton(tr("Auto white balance"), this);
    wbAuto->setIcon(icon(Icon::Wand, theme().text, 15));
    wbAuto->setToolTip(tr("Neutralise the bright background of the current image (F7)"));
    auto *wbPick = new QPushButton(tr("Pick area…"), this);
    wbPick->setIcon(icon(Icon::Pick, theme().text, 15));
    wbPick->setToolTip(tr("Drag a rectangle over an empty (white) area of the slide"));
    wbRow->addWidget(wbAuto, 1);
    wbRow->addWidget(wbPick);
    pre->contentLayout()->addLayout(wbRow);
    m_wbR = new SliderSpin(tr("Red"), 0.2, 5.0, 3, this, true);
    m_wbG = new SliderSpin(tr("Green"), 0.2, 5.0, 3, this, true);
    m_wbB = new SliderSpin(tr("Blue"), 0.2, 5.0, 3, this, true);
    for (auto *s : {m_wbR, m_wbG, m_wbB}) {
        s->setDefault(1.0);
        pre->contentLayout()->addWidget(s);
    }

    // --- light filter (emulates a colour filter in front of the lamp)
    auto *filt = group->addSection(tr("Light filter"), Icon::Lamp);
    m_filterPresets = new QComboBox(this);
    m_filterPresets->setToolTip(tr("Emulates a colour filter in front of the lamp. A halogen lamp looks slightly "
                                   "yellow; a blue (cooling) filter gives a cleaner white background and crisper "
                                   "stain contrast. Applied to the live image and to captures."));
    struct FilterPreset
    {
        QString name;
        double temp, tint;
    };
    const QList<FilterPreset> presets = {
        {tr("None (neutral)"), 0, 0},
        {tr("Slight blue (82)"), 12, 0},
        {tr("Light blue (82A)"), 25, 0},
        {tr("Medium blue (80C)"), 40, 0},
        {tr("Daylight blue (80B)"), 60, 0},
        {tr("Strong blue (80A)"), 85, 0},
        {tr("Cool white, slightly magenta"), 30, 10},
        {tr("Slight warm (81)"), -12, 0},
        {tr("Warm (81B)"), -25, 0},
        {tr("Warm halogen (85)"), -55, 0},
        {tr("Strong amber (85B)"), -85, 0},
        {tr("Green correction (CC10G)"), 0, -25},
        {tr("Magenta correction (CC10M)"), 0, 25},
    };
    for (const auto &p : presets)
        m_filterPresets->addItem(p.name, QPointF(p.temp, p.tint));
    m_filterPresets->addItem(tr("Custom"), QVariant());
    filt->contentLayout()->addWidget(m_filterPresets);
    m_filterTemp = new SliderSpin(tr("Warm ↔ Cool"), -100, 100, 0, this);
    m_filterTemp->setToolTip(tr("Negative: warmer (yellow / amber) light. Positive: cooler (bluer) light, "
                                "like a daylight-blue filter."));
    m_filterTemp->setDefault(0.0);
    m_filterTint = new SliderSpin(tr("Green ↔ Magenta"), -100, 100, 0, this);
    m_filterTint->setToolTip(tr("Negative: greener. Positive: more magenta (removes a green cast)."));
    m_filterTint->setDefault(0.0);
    filt->contentLayout()->addWidget(m_filterTemp);
    filt->contentLayout()->addWidget(m_filterTint);
    auto *filtNote = new QLabel(tr("For DAB / IHC measurements use the same filter for all images of a study."), this);
    filtNote->setWordWrap(true);
    filtNote->setEnabled(false);
    filt->contentLayout()->addWidget(filtNote);
    connect(m_filterPresets, &QComboBox::activated, this, [this](int i) {
        const QVariant v = m_filterPresets->itemData(i);
        if (!v.isValid())
            return; // "Custom": keep the sliders
        const QPointF p = v.toPointF();
        m_s.filterTemperature = p.x();
        m_s.filterTint = p.y();
        setSettings(m_s);
        emitChanged();
    });

    // --- tone
    auto *tone = group->addSection(tr("Brightness & contrast"), Icon::Contrast, false);
    auto *toneRow = new QHBoxLayout;
    auto *autoBlack = new QPushButton(tr("Black balance"), this);
    autoBlack->setIcon(icon(Icon::Wand, theme().text, 15));
    autoBlack->setToolTip(tr("Measures the camera's dark signal. Turn the lamp off (or close the light path) first. "
                             "Rarely needed."));
    auto *autoLevels = new QPushButton(tr("Auto levels"), this);
    autoLevels->setIcon(icon(Icon::Wand, theme().text, 15));
    autoLevels->setToolTip(tr("Automatically sets the black and white points for the best contrast of this image"));
    toneRow->addWidget(autoBlack);
    toneRow->addWidget(autoLevels);
    tone->contentLayout()->addLayout(toneRow);
    m_black = new SliderSpin(tr("Camera black level"), 0.0, 0.2, 4, this);
    m_black->setToolTip(tr("Signal subtracted as 'no light'. Set it with Black balance; normally leave it alone."));
    m_black->setDefault(0.002);
    m_blackPoint = new SliderSpin(tr("Black point"), 0.0, 0.9, 3, this);
    m_blackPoint->setDefault(0.0);
    m_whitePoint = new SliderSpin(tr("White point"), 0.1, 1.0, 3, this);
    m_whitePoint->setDefault(1.0);
    m_gamma = new SliderSpin(tr("Gamma"), 0.3, 3.0, 2, this, true);
    m_gamma->setToolTip(tr("Brightens (above 1) or darkens (below 1) the mid-tones without changing black and white"));
    m_gamma->setDefault(1.0);
    m_brightness = new SliderSpin(tr("Brightness"), -0.5, 0.5, 3, this);
    m_brightness->setDefault(0.0);
    m_contrast = new SliderSpin(tr("Contrast"), 0.3, 3.0, 2, this, true);
    m_contrast->setDefault(1.0);
    for (auto *s : {m_black, m_blackPoint, m_whitePoint, m_gamma, m_brightness, m_contrast})
        tone->contentLayout()->addWidget(s);

    // --- colour appearance
    auto *col = group->addSection(tr("Saturation & sharpness"), Icon::Sharpen, false);
    m_saturation = new SliderSpin(tr("Saturation"), 0.0, 3.0, 2, this);
    m_saturation->setDefault(1.0);
    m_hue = new SliderSpin(tr("Hue"), -180, 180, 0, this, false, QStringLiteral("°"));
    m_hue->setDefault(0.0);
    m_sharpen = new SliderSpin(tr("Sharpen"), 0.0, 3.0, 2, this);
    m_sharpen->setDefault(0.0);
    m_sharpenRadius = new SliderSpin(tr("Sharpen radius"), 0.5, 5.0, 1, this, false, tr(" px"));
    m_sharpenRadius->setDefault(1.0);
    for (auto *s : {m_saturation, m_hue, m_sharpen, m_sharpenRadius})
        col->contentLayout()->addWidget(s);
    m_ccm = new QCheckBox(tr("Camera colour correction"), this);
    m_ccm->setToolTip(tr("Calibrated colour matrix of the camera sensor (IMX174, halogen illumination): "
                         "accurate stain colours. Switch off only to compare with uncorrected sensor colours."));
    m_gray = new QCheckBox(tr("Monochrome"), this);
    m_invert = new QCheckBox(tr("Invert (negative)"), this);
    m_srgb = new QCheckBox(tr("Standard display brightness curve (recommended)"), this);
    m_srgb->setToolTip(tr("sRGB tone curve. Keep on for images that look natural on screen and in reports."));
    for (auto *c : {m_ccm, m_gray, m_invert, m_srgb})
        col->contentLayout()->addWidget(c);
    auto *reset = new QPushButton(tr("Reset all colour settings"), this);
    reset->setIcon(icon(Icon::Reset, theme().text, 15));
    col->contentLayout()->addWidget(reset);

    reloadPresets();

    auto hook = [this](SliderSpin *s, double ColorSettings::*field) {
        connect(s, &SliderSpin::valueChanged, this, [this, field](double v) {
            if (m_updating)
                return;
            m_s.*field = v;
            emitChanged();
        });
    };
    hook(m_wbR, &ColorSettings::wbRed);
    hook(m_wbG, &ColorSettings::wbGreen);
    hook(m_wbB, &ColorSettings::wbBlue);
    hook(m_black, &ColorSettings::blackLevel);
    hook(m_blackPoint, &ColorSettings::blackPoint);
    hook(m_whitePoint, &ColorSettings::whitePoint);
    hook(m_gamma, &ColorSettings::gamma);
    hook(m_brightness, &ColorSettings::brightness);
    hook(m_contrast, &ColorSettings::contrast);
    hook(m_saturation, &ColorSettings::saturation);
    hook(m_hue, &ColorSettings::hue);
    hook(m_sharpen, &ColorSettings::sharpenAmount);
    hook(m_sharpenRadius, &ColorSettings::sharpenRadius);
    hook(m_filterTemp, &ColorSettings::filterTemperature);
    hook(m_filterTint, &ColorSettings::filterTint);
    for (auto *s : {m_filterTemp, m_filterTint})
        connect(s, &SliderSpin::valueChanged, this, [this] {
            if (!m_updating)
                syncFilterPreset();
        });
    auto hookB = [this](QCheckBox *c, bool ColorSettings::*field) {
        connect(c, &QCheckBox::toggled, this, [this, field](bool on) {
            if (m_updating)
                return;
            m_s.*field = on;
            emitChanged();
        });
    };
    hookB(m_gray, &ColorSettings::grayscale);
    hookB(m_ccm, &ColorSettings::colorCorrection);
    hookB(m_invert, &ColorSettings::invert);
    hookB(m_srgb, &ColorSettings::srgbEncode);

    connect(wbAuto, &QPushButton::clicked, this, &ColorPanel::autoWhiteBalanceRequested);
    connect(wbPick, &QPushButton::clicked, this, &ColorPanel::pickWhiteBalanceRequested);
    connect(autoBlack, &QPushButton::clicked, this, &ColorPanel::autoBlackRequested);
    connect(autoLevels, &QPushButton::clicked, this, &ColorPanel::autoLevelsRequested);
    connect(reset, &QPushButton::clicked, this, [this] {
        ColorSettings c;
        // keep orientation, it belongs to the optical setup
        c.flipHorizontal = m_s.flipHorizontal;
        c.flipVertical = m_s.flipVertical;
        c.rotation = m_s.rotation;
        setSettings(c);
        emitChanged();
    });
    connect(m_presets, &QComboBox::activated, this, [this](int i) {
        const QString name = m_presets->itemText(i);
        auto &S = AppSettings::instance();
        const auto builtin = AppSettings::builtinPresets();
        ColorSettings c = S.colorPresets.contains(name) ? S.colorPresets.value(name) : builtin.value(name);
        // presets never change white balance, light filter or orientation (depend on lamp and optics)
        c.wbRed = m_s.wbRed;
        c.wbGreen = m_s.wbGreen;
        c.wbBlue = m_s.wbBlue;
        c.filterTemperature = m_s.filterTemperature;
        c.filterTint = m_s.filterTint;
        c.flipHorizontal = m_s.flipHorizontal;
        c.flipVertical = m_s.flipVertical;
        c.rotation = m_s.rotation;
        setSettings(c);
        emitChanged();
    });
    connect(savePreset, &QToolButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save colour preset"), tr("Preset name:"),
                                                   QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        AppSettings::instance().colorPresets.insert(name.trimmed(), m_s);
        AppSettings::instance().save();
        reloadPresets();
        m_presets->setCurrentText(name.trimmed());
    });
    connect(delPreset, &QToolButton::clicked, this, [this] {
        const QString name = m_presets->currentText();
        auto &S = AppSettings::instance();
        if (!S.colorPresets.contains(name)) {
            QMessageBox::information(this, tr("Presets"), tr("Built-in presets cannot be deleted."));
            return;
        }
        S.colorPresets.remove(name);
        S.save();
        reloadPresets();
    });
}

void ColorPanel::reloadPresets()
{
    m_presets->clear();
    for (const auto &k : AppSettings::builtinPresets().keys())
        m_presets->addItem(k);
    const auto user = AppSettings::instance().colorPresets.keys();
    if (!user.isEmpty())
        m_presets->insertSeparator(m_presets->count());
    for (const auto &k : user)
        m_presets->addItem(k);
    m_presets->setCurrentIndex(-1);
    m_presets->setPlaceholderText(tr("Presets…"));
}

void ColorPanel::setSettings(const ColorSettings &s)
{
    m_updating = true;
    m_s = s;
    m_wbR->setValue(s.wbRed);
    m_wbG->setValue(s.wbGreen);
    m_wbB->setValue(s.wbBlue);
    m_black->setValue(s.blackLevel);
    m_blackPoint->setValue(s.blackPoint);
    m_whitePoint->setValue(s.whitePoint);
    m_gamma->setValue(s.gamma);
    m_brightness->setValue(s.brightness);
    m_contrast->setValue(s.contrast);
    m_saturation->setValue(s.saturation);
    m_hue->setValue(s.hue);
    m_sharpen->setValue(s.sharpenAmount);
    m_sharpenRadius->setValue(s.sharpenRadius);
    m_gray->setChecked(s.grayscale);
    m_ccm->setChecked(s.colorCorrection);
    m_invert->setChecked(s.invert);
    m_srgb->setChecked(s.srgbEncode);
    m_filterTemp->setValue(s.filterTemperature);
    m_filterTint->setValue(s.filterTint);
    syncFilterPreset();
    m_updating = false;
}

void ColorPanel::syncFilterPreset()
{
    // select the preset matching the sliders, otherwise "Custom"
    const QSignalBlocker block(m_filterPresets);
    int custom = m_filterPresets->count() - 1;
    for (int i = 0; i < m_filterPresets->count(); ++i) {
        const QVariant v = m_filterPresets->itemData(i);
        if (!v.isValid()) {
            custom = i;
            continue;
        }
        const QPointF p = v.toPointF();
        if (std::abs(p.x() - m_s.filterTemperature) < 0.5 && std::abs(p.y() - m_s.filterTint) < 0.5) {
            m_filterPresets->setCurrentIndex(i);
            return;
        }
    }
    m_filterPresets->setCurrentIndex(custom);
}

void ColorPanel::setWhiteBalance(double r, double g, double b)
{
    m_s.wbRed = r;
    m_s.wbGreen = g;
    m_s.wbBlue = b;
    setSettings(m_s);
    emitChanged();
}

void ColorPanel::setBlackLevel(double v)
{
    m_s.blackLevel = v;
    setSettings(m_s);
    emitChanged();
}

void ColorPanel::setLevels(double black, double white)
{
    m_s.blackPoint = black;
    m_s.whitePoint = white;
    setSettings(m_s);
    emitChanged();
}

void ColorPanel::emitChanged()
{
    emit settingsChanged(m_s);
}

} // namespace lm
