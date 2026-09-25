#pragma once
// Camera selection, live control, exposure/gain/resolution and orientation.

#include "camera/Camera.h"

#include <QWidget>

class QComboBox;
class QPushButton;
class QCheckBox;
class QLabel;
class QFormLayout;

namespace lm {

class AcquisitionEngine;
class SliderSpin;
class CollapsibleSection;

class CameraPanel : public QWidget {
    Q_OBJECT
public:
    explicit CameraPanel(AcquisitionEngine *engine, QWidget *parent = nullptr);

    void refreshCameras();
    bool connectCamera(int index);
    void autoConnect();
    void syncFromCamera();
    void setExposureDisplay(double ms, double gain);
    std::vector<CameraInfo> cameras() const { return m_cameras; }

signals:
    void cameraChanged();          // opened / closed
    void orientationChanged();     // flip/rotate buttons
    void message(const QString &text, int timeoutMs);

private slots:
    void onConnectClicked();
    void onLiveToggled(bool on);

private:
    void buildAdvanced();
    void updateEnabled();

    AcquisitionEngine *m_engine;
    std::vector<CameraInfo> m_cameras;
    QComboBox *m_cameraCombo;
    QPushButton *m_connect;
    QPushButton *m_live;
    QPushButton *m_freeze;
    QLabel *m_info;
    QComboBox *m_resolution;
    SliderSpin *m_exposure;
    QCheckBox *m_autoExposure;
    SliderSpin *m_aeTarget;
    QPushButton *m_aeOnce;
    SliderSpin *m_gain;
    QCheckBox *m_aeGain;
    QCheckBox *m_hqPreview;
    CollapsibleSection *m_advanced;
    QFormLayout *m_advancedForm = nullptr;
};

} // namespace lm
