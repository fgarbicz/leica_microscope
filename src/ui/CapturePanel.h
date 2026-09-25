#pragma once
// Capture controls: single images, pixel shift, time lapse, extended depth of
// field (Multifocus) and live image stitching.

#include <QWidget>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QProgressBar;

namespace lm {

class CapturePanel : public QWidget {
    Q_OBJECT
public:
    explicit CapturePanel(QWidget *parent = nullptr);

    void setShotModes(const QStringList &names); // camera multi-shot modes
    void setBusy(bool busy, const QString &what = QString());
    void setProgress(int done, int total);
    void refreshFromSettings();
    void updateNextName(const QString &name);

    // state updates from the main window
    void setTimelapseRunning(bool on, int done, int total);
    void setMultifocusRunning(bool on, int frames);
    void setMosaicRunning(bool on, int tiles, bool tracking);

signals:
    void captureRequested();
    void timelapseToggled(bool start);
    void multifocusStart();
    void multifocusFinish();
    void multifocusCancel();
    void mosaicStart();
    void mosaicFinish();
    void mosaicCancel();
    void mosaicAddTile();
    void mosaicAutoAddChanged(bool on);
    void settingsChanged();

private:
    void store();

    QPushButton *m_capture;
    QProgressBar *m_progress;
    QLabel *m_nextName;
    QLineEdit *m_folder;
    QLineEdit *m_sample;
    QLineEdit *m_pattern;
    QSpinBox *m_counter;
    QComboBox *m_format;
    QSpinBox *m_jpegQuality;
    QSpinBox *m_average;
    QComboBox *m_mode;
    QCheckBox *m_burnScale;
    QCheckBox *m_openProcess;
    QCheckBox *m_prompt;
    // time lapse
    QDoubleSpinBox *m_tlInterval;
    QSpinBox *m_tlCount;
    QPushButton *m_tlStart;
    QLabel *m_tlStatus;
    // multifocus
    QPushButton *m_mfStart, *m_mfFinish, *m_mfCancel;
    QLabel *m_mfStatus;
    // mosaic
    QPushButton *m_moStart, *m_moFinish, *m_moCancel, *m_moAdd;
    QCheckBox *m_moAuto;
    QLabel *m_moStatus;
    bool m_updating = false;
};

} // namespace lm
