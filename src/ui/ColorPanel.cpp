#include "ColorPanel.h"

#include "app/AppSettings.h"
#include "ui/CollapsibleSection.h"
#include "ui/SliderSpin.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace lm {

ColorPanel::ColorPanel(QWidget *parent) : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- presets
    auto *pre = new CollapsibleSection(tr("Colour"), this);
    auto *prow = new QHBoxLayout;
    m_presets = new QComboBox(this);
    auto *savePreset = new QToolButton(this);
    savePreset->setText(QStringLiteral("＋"));
    savePreset->setToolTip(tr("Save current settings as a preset"));
    auto *delPreset = new QToolButton(this);
    delPreset->setText(QStringLiteral("－"));
    delPreset->setToolTip(tr("Delete the selected user preset"));
    prow->addWidget(m_presets, 1);
    prow->addWidget(savePreset);
    prow->addWidget(delPreset);
    pre->contentLayout()->addLayout(prow);

    // white balance
    auto *wbRow = new QHBoxLayout;
    auto *wbAuto = new QPushButton(tr("Auto white balance"), this);
    wbAuto->setToolTip(tr("Neutralise the bright background of the current image (F7)"));
    auto *wbPick = new QPushButton(tr("Pick area…"), this);
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
    root->addWidget(pre);

    // --- tone
    auto *tone = new CollapsibleSection(tr("Brightness & contrast"), this);
    auto *toneRow = new QHBoxLayout;
    auto *autoBlack = new QPushButton(tr("Black balance"), this);
    autoBlack->setToolTip(tr("Measure the sensor black level (close the light path first)"));
    auto *autoLevels = new QPushButton(tr("Auto levels"), this);
    autoLevels->setToolTip(tr("Stretch the histogram of the current image"));
    toneRow->addWidget(autoBlack);
    toneRow->addWidget(autoLevels);
    tone->contentLayout()->addLayout(toneRow);
    m_black = new SliderSpin(tr("Black level (offset)"), 0.0, 0.2, 4, this);
    m_black->setDefault(0.002);
    m_blackPoint = new SliderSpin(tr("Black point"), 0.0, 0.9, 3, this);
    m_blackPoint->setDefault(0.0);
    m_whitePoint = new SliderSpin(tr("White point"), 0.1, 1.0, 3, this);
    m_whitePoint->setDefault(1.0);
    m_gamma = new SliderSpin(tr("Gamma"), 0.3, 3.0, 2, this, true);
    m_gamma->setDefault(1.0);
    m_brightness = new SliderSpin(tr("Brightness"), -0.5, 0.5, 3, this);
    m_brightness->setDefault(0.0);
    m_contrast = new SliderSpin(tr("Contrast"), 0.3, 3.0, 2, this, true);
    m_contrast->setDefault(1.0);
    for (auto *s : {m_black, m_blackPoint, m_whitePoint, m_gamma, m_brightness, m_contrast})
        tone->contentLayout()->addWidget(s);
    root->addWidget(tone);

    // --- colour appearance
    auto *col = new CollapsibleSection(tr("Saturation & sharpness"), this);
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
    m_srgb = new QCheckBox(tr("sRGB tone curve (recommended)"), this);
    for (auto *c : {m_ccm, m_gray, m_invert, m_srgb})
        col->contentLayout()->addWidget(c);
    auto *reset = new QPushButton(tr("Reset all colour settings"), this);
    col->contentLayout()->addWidget(reset);
    root->addWidget(col);

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
        // presets never change white balance or orientation (depend on lamp and optics)
        c.wbRed = m_s.wbRed;
        c.wbGreen = m_s.wbGreen;
        c.wbBlue = m_s.wbBlue;
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
    m_updating = false;
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
