#pragma once

#include "app/AcquisitionEngine.h"
#include "app/Calibration.h"
#include "io/Metadata.h"

#include <QMainWindow>
#include <QPointer>
#include <QTimer>

class QTabBar;
class QStackedWidget;
class QLabel;
class QSplitter;

namespace lm {

class ImageView;
class CameraPanel;
class ColorPanel;
class MicroscopePanel;
class CapturePanel;
class ToolsPanel;
class GalleryWidget;
class BrowsePage;
class ProcessPage;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    void startup();

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private slots:
    void onFrame(const QImage &img, const lm::LiveStats &stats);
    void onCaptureFinished(std::shared_ptr<lm::CaptureResult> r);
    void capture();
    void onCameraChanged();
    void onCalibrationChanged();
    void onTimelapseTick();
    void onCursorMoved(const QPoint &p, bool inside);

private:
    QWidget *buildAcquirePage();
    void buildMenus();
    void setWorkspace(int index);
    void applyColorSettings(const ColorSettings &c);
    void loadShadingForObjective();
    void startCalibration();
    ImageMetadata currentMetadata(const CaptureResult &r) const;
    void showMessage(const QString &text, int timeoutMs = 4000);
    void updateTitle();
    void updateNextName();
    void onCameraLost(const QString &reason);
    void tryReconnect();

    AcquisitionEngine *m_engine;
    MicroscopeConfig m_scope;

    QTabBar *m_tabs;
    QStackedWidget *m_stack;
    ImageView *m_view;
    CameraPanel *m_cameraPanel;
    ColorPanel *m_colorPanel;
    MicroscopePanel *m_scopePanel;
    CapturePanel *m_capturePanel;
    ToolsPanel *m_toolsPanel;
    GalleryWidget *m_gallery;
    BrowsePage *m_browse;
    ProcessPage *m_process;

    QLabel *m_statusCamera, *m_statusFps, *m_statusExposure, *m_statusCursor, *m_statusZoom;
    QTimer m_timelapse;
    QTimer m_reconnect;   // polls for a lost camera
    QString m_lostCameraId;
    int m_timelapseDone = 0;
    bool m_multifocus = false, m_mosaic = false;
    bool m_capturing = false;
    QList<QPoint> m_calibPoints;
    LiveStats m_lastStats;
};

} // namespace lm
