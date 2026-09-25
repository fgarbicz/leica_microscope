#pragma once
// Microscope configuration: objective selection, spatial calibration and
// shading (flat-field) correction.

#include <QWidget>

class QComboBox;
class QLabel;
class QCheckBox;
class QPushButton;
class QDoubleSpinBox;

namespace lm {

class MicroscopeConfig;
class CollapsibleSection;

class MicroscopePanel : public QWidget {
    Q_OBJECT
public:
    MicroscopePanel(MicroscopeConfig *config, QWidget *parent = nullptr);

    void refresh();
    void setShadingStatus(const QString &text, bool available);
    bool shadingEnabled() const;
    void setShadingEnabled(bool on);

signals:
    void calibrationChanged();                // objective or µm/px changed
    void calibrateRequested();                // start stage micrometer calibration
    void shadingReferenceRequested();
    void shadingEnabledChanged(bool on);
    void shadingClearRequested();

private:
    void editObjectives();

    MicroscopeConfig *m_cfg;
    QComboBox *m_objective;
    QDoubleSpinBox *m_adapter;
    QLabel *m_scaleInfo;
    QLabel *m_shadingInfo;
    QCheckBox *m_shading;
    QPushButton *m_shadingClear;
    CollapsibleSection *m_section = nullptr; // for the collapsed summary
};

} // namespace lm
