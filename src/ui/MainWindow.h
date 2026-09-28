#pragma once

#include "app/AcquisitionEngine.h"
#include "app/Calibration.h"
#include "app/VideoRecorder.h"
#include "io/Metadata.h"

#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QTimer>

class QTabBar;
class QPushButton;
class QToolButton;
class QStackedWidget;
class QLabel;
class QSplitter;
class QScrollArea;
class QAction;

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
    // Files given on the command line, by the desktop (a .desktop file's %F,
    // "Open with" in Explorer) or by the Finder (QFileOpenEvent): the first
    // image opens in Process; a folder opens in Browse.
    void openPaths(const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;
    void updatePreviewVisibility();
    void applyLiveDab();
    bool eventFilter(QObject *o, QEvent *e) override;

private slots:
    void onFrame(const QImage &img, const lm::LiveStats &stats);
    void onCaptureFinished(std::shared_ptr<lm::CaptureResult> r);
    void capture();
    // "Capturing…" / "Saving…" in the middle of the live image while either runs;
    // called whenever either changes
    void updateCaptureState();
    void onCameraChanged();
    void onCalibrationChanged();
    void onTimelapseTick();
    void onCursorMoved(const QPoint &p, bool inside);

private:
    QWidget *buildAcquirePage();
    void buildMenus();
    void setWorkspace(int index);
    // Applies a theme and interface size to the running application, and
    // optionally remembers them (the Settings dialog saves on OK itself).
    void applyAppearance(const QString &theme, int scalePercent, bool save = false);
    void applyColorSettings(const ColorSettings &c);
    // Captured images beside the live image (vertical list) or under it (reel).
    // Writes this session's captures into one Leica .lif, the way LAS X keeps
    // a session.
    void exportSessionToLif();
    void applyGalleryLayout(bool vertical);
    // how the captured images are shown: the reel under the image, the list
    // beside it, or the compact list (small thumbnails, one line each)
    enum GalleryMode { GalleryReel = 0, GalleryList = 1, GalleryCompact = 2 };
    void setGalleryMode(int mode);
    void syncGalleryControls(int mode); // menu items and header buttons
    void loadShadingForObjective();
    void startCalibration();
    ImageMetadata currentMetadata(const CaptureResult &r) const;
    void showMessage(const QString &text, int timeoutMs = 4000);
    void updateTitle();
    void updateNextName();
    void onCameraLost(const QString &reason);
    void tryReconnect();
    // blends a saved image over the live view (empty path = hide)
    void showReference(const QString &path);
    // pane sizes are remembered between sessions (per gallery layout for the centre)
    QString centreSplitKey() const;
    void saveSplitters();
    void restoreSplitters();

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
    QAction *m_referenceAct = nullptr;
    QString m_referencePath;

    QLabel *m_statusCamera, *m_statusFps, *m_statusExposure, *m_statusCursor, *m_statusZoom;
    // A coloured dot in the status bar: grey none, blue connected, green live,
    // red lost. Visible from across the room, unlike a word.
    enum class CameraState { None, Ready, Live, Lost };
    QLabel *m_statusLed = nullptr;
    CameraState m_cameraState = CameraState::None;
    void setCameraLed(CameraState state);
    QScrollArea *m_leftPanel = nullptr;   // the two side panels, so their minimum
    QScrollArea *m_rightPanel = nullptr;  // width can follow the interface size
    QSplitter *m_acquireSplitter = nullptr;
    QSplitter *m_centreSplitter = nullptr; // live image + captured images
    // the two layouts of the captured images, as View menu items and as the
    // two buttons on the strip's own header; setGalleryVertical() keeps all in step
    QAction *m_galleryModeAct[3] = {};
    QToolButton *m_galleryModeBtn[3] = {};
    QTimer m_timelapse;
    QTimer m_reconnect;   // polls for a lost camera
    VideoRecorder m_recorder;
    QTimer m_recTimer;
    QString m_lostCameraId;
    int m_timelapseDone = 0;
    bool m_multifocus = false, m_mosaic = false;
    bool m_capturing = false;
    int m_captureSerial = 0; // identifies the capture a watchdog timer belongs to
    QList<QPointF> m_calibPoints; // in sensor pixels
    bool m_resetSettingsOnClose = false; // Settings → Reset all settings is closing the application
    // the image of the workspace on screen, for the View menu's zoom commands
    ImageView *currentImageView() const;
    QSet<QString> m_pendingSaves; // images being written in the background
    LiveStats m_lastStats;
    QSize m_liveFrameSize; // size actually delivered, to spot a mis-reporting camera
    int m_prevObjective = -1;
    bool m_objectiveFromCapture = false;
    void storeObjectiveSettings(int index);
    void restoreObjectiveSettings(int index);
};

} // namespace lm
