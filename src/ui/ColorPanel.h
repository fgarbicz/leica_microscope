#pragma once
// Colour & image formation controls (white balance, levels, gamma, ...).

#include "imaging/ColorPipeline.h"

#include <QWidget>

class QComboBox;
class QCheckBox;
class QPushButton;

namespace lm {

class SliderSpin;

class ColorPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColorPanel(QWidget *parent = nullptr);

    ColorSettings settings() const { return m_s; }
    void setSettings(const ColorSettings &s); // updates controls, emits nothing
    void setWhiteBalance(double r, double g, double b);
    void setBlackLevel(double v);
    void setLevels(double black, double white);

signals:
    void settingsChanged(const lm::ColorSettings &s);
    void autoWhiteBalanceRequested();
    void pickWhiteBalanceRequested();
    void autoBlackRequested();
    void autoLevelsRequested();

private:
    void emitChanged();
    void reloadPresets();

    ColorSettings m_s;
    bool m_updating = false;
    QComboBox *m_presets;
    SliderSpin *m_wbR, *m_wbG, *m_wbB;
    SliderSpin *m_black;
    SliderSpin *m_blackPoint, *m_whitePoint;
    SliderSpin *m_gamma, *m_brightness, *m_contrast, *m_saturation, *m_hue;
    SliderSpin *m_sharpen, *m_sharpenRadius;
    QCheckBox *m_gray, *m_invert, *m_srgb, *m_ccm;
};

} // namespace lm
