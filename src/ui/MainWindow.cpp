#include "MainWindow.h"

#include "app/AppSettings.h"
#include "io/ImageIO.h"
#include "ui/BrowsePage.h"
#include "ui/CameraPanel.h"
#include "ui/CaptureDialog.h"
#include "ui/CapturePanel.h"
#include "ui/ColorPanel.h"
#include "ui/CompareWindow.h"
#include "ui/GalleryWidget.h"
#include "ui/HistogramWidget.h"
#include "ui/Icons.h"
#include "ui/ImageView.h"
#include "ui/MicroscopePanel.h"
#include "ui/Overlays.h"
#include "ui/PlatformUi.h"
#include "ui/ProcessPage.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"
#include "ui/ToolsPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QThreadPool>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>

namespace lm {

namespace {
// Wide enough for the longest control label in every platform font (the
// interface font is larger on macOS and Linux than on Windows).
constexpr int kPanelWidth = 340;

QScrollArea *panelScroll(QWidget *content, int minWidth)
{
    auto *sa = new QScrollArea;
    sa->setObjectName(QStringLiteral("PanelScroll"));
    sa->setWidgetResizable(true);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->setMinimumWidth(minWidth);
    content->setObjectName(QStringLiteral("PanelContent"));
    sa->setWidget(content);
    return sa;
}

QString shortObjective(const Objective &o)
{
    return QStringLiteral("%1x").arg(o.magnification);
}
} // namespace

MainWindow::MainWindow()
{
    auto &S = AppSettings::instance();
    m_scope.load();
    m_engine = new AcquisitionEngine(this);
    m_engine->setColorSettings(S.color);

    setWindowTitle(QStringLiteral("DM Imaging"));
    setMinimumSize(1100, 700);

    // ---- top bar with workflow tabs
    auto *central = new QWidget(this);
    auto *cl = new QVBoxLayout(central);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    auto *top = new QWidget(central);
    top->setObjectName(QStringLiteral("TopBar"));
    auto *tl = new QHBoxLayout(top);
    tl->setContentsMargins(0, 0, 8, 0);
    auto *logo = new QLabel(top);
    logo->setPixmap(QIcon(QStringLiteral(":/icons/app.png")).pixmap(20, 20));
    logo->setContentsMargins(12, 0, 0, 0);
    tl->addWidget(logo);
    auto *title = new QLabel(QStringLiteral("DM Imaging"), top);
    title->setObjectName(QStringLiteral("AppTitle"));
    tl->addWidget(title);
    m_tabs = new QTabBar(top);
    m_tabs->setObjectName(QStringLiteral("WorkflowTabs"));
    // The three stages of the workflow, left to right, each with its own symbol.
    m_tabs->addTab(icon(Icon::Acquire, theme().subText, 16), tr("Acquire"));
    m_tabs->addTab(icon(Icon::Browse, theme().subText, 16), tr("Browse"));
    m_tabs->addTab(icon(Icon::Process, theme().subText, 16), tr("Process"));
    m_tabs->setTabToolTip(0, tr("Live image, camera settings and capturing (Alt+1)"));
    m_tabs->setTabToolTip(1, tr("The images you have captured (Alt+2)"));
    m_tabs->setTabToolTip(2, tr("Measure, quantify and export one image (Alt+3)"));
    m_tabs->setIconSize(QSize(16, 16));
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    // without this the bar shrinks to its minimum and hides tabs behind arrows
    m_tabs->setUsesScrollButtons(false);
    m_tabs->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    tl->addWidget(m_tabs);
    tl->addStretch();
    cl->addWidget(top);

    m_stack = new QStackedWidget(central);
    m_stack->addWidget(buildAcquirePage());
    m_browse = new BrowsePage(m_stack);
    m_stack->addWidget(m_browse);
    m_process = new ProcessPage(m_stack);
    m_stack->addWidget(m_process);
    cl->addWidget(m_stack, 1);
    setCentralWidget(central);

    // ---- status bar
    m_statusCamera = new QLabel(tr("No camera"));
    m_statusFps = new QLabel;
    m_statusExposure = new QLabel;
    m_statusCursor = new QLabel;
    m_statusZoom = new QLabel;
    statusBar()->addPermanentWidget(m_statusCursor);
    statusBar()->addPermanentWidget(m_statusZoom);
    statusBar()->addPermanentWidget(m_statusExposure);
    statusBar()->addPermanentWidget(m_statusFps);
    statusBar()->addPermanentWidget(m_statusCamera);

    buildMenus();

    connect(m_tabs, &QTabBar::currentChanged, this, &MainWindow::setWorkspace);
    connect(m_engine, &AcquisitionEngine::frameReady, this, &MainWindow::onFrame, Qt::QueuedConnection);
    connect(m_engine, &AcquisitionEngine::captureFinished, this, &MainWindow::onCaptureFinished, Qt::QueuedConnection);
    connect(m_engine, &AcquisitionEngine::captureFailed, this, [this](const QString &m) {
        m_capturing = false;
        m_capturePanel->setBusy(false);
        m_cameraPanel->setBusy(false);
        if (m_timelapse.isActive()) { // do not interrupt a time lapse with dialogs
            showMessage(tr("Capture failed: %1").arg(m), 8000);
            return;
        }
        QMessageBox::warning(this, tr("Capture"), m);
    });
    connect(m_engine, &AcquisitionEngine::captureProgress, this, [this](int done, int total, const QString &what) {
        m_capturePanel->setProgress(done, total);
        showMessage(tr("%1: %2 / %3").arg(what).arg(done).arg(total), 0);
    });
    connect(m_engine, &AcquisitionEngine::cameraError, this, &MainWindow::onCameraLost);
    connect(m_engine, &AcquisitionEngine::processingError, this, [this](const QString &msg) {
        qWarning("processing error: %s", qPrintable(msg));
        showMessage(tr("Image processing error: %1").arg(msg), 8000);
    });
    m_reconnect.setInterval(2000);
    connect(&m_reconnect, &QTimer::timeout, this, &MainWindow::tryReconnect);
    connect(m_engine, &AcquisitionEngine::whiteBalanceComputed, this, [this](double r, double g, double b) {
        m_colorPanel->setWhiteBalance(r, g, b);
        showMessage(tr("White balance: R %1  G %2  B %3").arg(r, 0, 'f', 3).arg(g, 0, 'f', 3).arg(b, 0, 'f', 3));
    });
    connect(m_engine, &AcquisitionEngine::blackLevelComputed, this, [this](double v) {
        m_colorPanel->setBlackLevel(v);
        showMessage(tr("Black level set to %1").arg(v, 0, 'f', 4));
    });
    connect(m_engine, &AcquisitionEngine::levelsComputed, this, [this](double b, double w) {
        m_colorPanel->setLevels(b, w);
        m_toolsPanel->setLevels(b, w);
    });
    connect(m_engine, &AcquisitionEngine::shadingReferenceReady, this, [this](std::shared_ptr<ShadingCorrection> sc) {
        if (!sc)
            return;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/shading");
        QDir().mkpath(dir);
        Objective &o = m_scope.currentObjective();
        const QString file = dir + QStringLiteral("/objective_%1_%2x.lmsh").arg(o.id).arg(o.magnification);
        if (!sc->save(file.toStdString())) {
            QMessageBox::warning(this, tr("Shading correction"), tr("The reference could not be saved to %1.").arg(file));
            return;
        }
        o.shadingFile = file;
        m_scope.save();
        m_engine->setShading(sc);
        m_scopePanel->setShadingEnabled(true);
        loadShadingForObjective();
        showMessage(tr("Shading reference acquired (max. correction %1×)").arg(sc->maxGain(), 0, 'f', 2), 6000);
        if (sc->maxGain() > 3.0)
            QMessageBox::information(this, tr("Shading correction"),
                                     tr("The reference is very uneven (correction up to %1×). Check that the field was "
                                        "empty and the illumination (Köhler) is centred.").arg(sc->maxGain(), 0, 'f', 1));
    });
    connect(m_engine, &AcquisitionEngine::multifocusProgress, this, [this](int n, double) {
        if (m_multifocus) // ignore updates that arrive after Finish/Cancel
            m_capturePanel->setMultifocusRunning(true, n);
    });
    connect(m_engine, &AcquisitionEngine::mosaicStatus, this, [this](const MosaicBuilder::Status &st) {
        if (!m_mosaic)
            return;
        m_capturePanel->setMosaicRunning(true, st.tiles, st.tracking);
        const double s = m_lastStats.displayScale;
        Camera *cam = m_engine->camera();
        if (cam && !cam->resolutions().empty()) {
            const auto r = cam->resolutions()[size_t(cam->resolutionIndex())];
            m_view->setHighlightRect(QRectF(st.posX * s, st.posY * s, r.width * s, r.height * s),
                                     st.tracking ? QColor(0, 200, 255) : QColor(230, 70, 60));
        }
        QVector<QRectF> tiles;
        for (const auto &t : m_engine->mosaic().tiles())
            tiles.push_back(QRectF(t.x * s, t.y * s, t.w * s, t.h * s));
        m_view->setTileRects(tiles);
    });
    connect(&m_timelapse, &QTimer::timeout, this, &MainWindow::onTimelapseTick);
    connect(m_engine, &AcquisitionEngine::liveStateChanged, this, [this](bool live) {
        if (!live && m_timelapse.isActive()) {
            m_timelapse.stop();
            const auto &c = AppSettings::instance().capture;
            m_capturePanel->setTimelapseRunning(false, m_timelapseDone, c.timelapseCount);
            showMessage(tr("Time lapse stopped: the live image was stopped"), 8000);
        }
    });
    connect(m_process, &ProcessPage::message, this, &MainWindow::showMessage);
    connect(m_browse, &BrowsePage::openInProcess, this, [this](const QString &p) {
        if (m_process->openFile(p))
            m_tabs->setCurrentIndex(2);
    });
    connect(m_gallery, &GalleryWidget::openRequested, this, [this](const QString &p) {
        if (m_process->openFile(p))
            m_tabs->setCurrentIndex(2);
    });

    const QSettings qs;
    restoreGeometry(qs.value(QStringLiteral("ui/geometry")).toByteArray());
    loadShadingForObjective();
    onCalibrationChanged();
    updateNextName();
}

MainWindow::~MainWindow() = default;

QWidget *MainWindow::buildAcquirePage()
{
    auto &S = AppSettings::instance();
    auto *page = new QWidget;
    auto *lay = new QHBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    auto *split = new QSplitter(Qt::Horizontal, page);
    split->setObjectName(QStringLiteral("AcquireSplitter"));

    // left: camera, microscope, capture
    auto *left = new QWidget;
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(0);
    m_cameraPanel = new CameraPanel(m_engine, left);
    m_scopePanel = new MicroscopePanel(&m_scope, left);
    m_capturePanel = new CapturePanel(left);
    // workflow order: connect the camera, set the exposure, pick the objective,
    // then capture
    ll->addWidget(m_cameraPanel);
    ll->addWidget(m_scopePanel);
    ll->addWidget(m_capturePanel);
    ll->addStretch();
    split->addWidget(panelScroll(left, kPanelWidth));

    // centre: image + gallery
    auto *centre = new QSplitter(Qt::Vertical, split);
    m_view = new ImageView(centre);
    m_view->setPlaceholder(tr("No camera connected\n\nConnect the camera in the Camera panel (or use the simulator)."));
    m_view->installEventFilter(this);
    m_gallery = new GalleryWidget(centre);
    centre->addWidget(m_view);
    centre->addWidget(m_gallery);
    centre->setStretchFactor(0, 5);
    centre->setStretchFactor(1, 1);
    centre->setSizes({800, 150});
    split->addWidget(centre);

    // right: what the image is (histogram, focus, information), then how it is
    // adjusted, then what is drawn over it
    auto *right = new QWidget;
    auto *rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    m_toolsPanel = new ToolsPanel(right);
    m_colorPanel = new ColorPanel(right);
    m_colorPanel->setSettings(S.color);
    rl->addWidget(m_toolsPanel);
    rl->addWidget(m_colorPanel);
    rl->addWidget(m_toolsPanel->overlaysPanel());
    rl->addStretch();
    split->addWidget(panelScroll(right, kPanelWidth));

    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    split->setSizes({kPanelWidth + 20, 1100, kPanelWidth + 20});
    lay->addWidget(split);

    // --- wiring of the panels
    connect(m_cameraPanel, &CameraPanel::message, this, &MainWindow::showMessage);
    connect(m_cameraPanel, &CameraPanel::cameraChanged, this, &MainWindow::onCameraChanged);
    connect(m_cameraPanel, &CameraPanel::orientationChanged, this, [this] {
        m_colorPanel->setSettings(m_engine->colorSettings());
    });
    connect(m_colorPanel, &ColorPanel::settingsChanged, this, &MainWindow::applyColorSettings);
    connect(m_colorPanel, &ColorPanel::autoWhiteBalanceRequested, this, [this] { m_engine->requestWhiteBalance(); });
    connect(m_colorPanel, &ColorPanel::pickWhiteBalanceRequested, this, [this] {
        m_view->startPick(ImageView::PickMode::Region, tr("Drag a rectangle over an empty (white) area — Esc cancels"));
        disconnect(m_view, &ImageView::regionPicked, nullptr, nullptr);
        connect(m_view, &ImageView::regionPicked, this, [this](const QRect &r) {
            const double s = m_lastStats.displayScale > 0 ? m_lastStats.displayScale : 1.0;
            m_engine->requestWhiteBalance({int(r.x() / s), int(r.y() / s), int(r.width() / s), int(r.height() / s)});
        }, Qt::SingleShotConnection);
    });
    connect(m_colorPanel, &ColorPanel::autoBlackRequested, this, [this] {
        if (QMessageBox::question(this, tr("Black balance"),
                                  tr("Close the light path (lamp off or shutter closed), then press OK.\n"
                                     "The darkest pixels of the image define the black level."),
                                  QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok)
            m_engine->requestBlackBalance();
    });
    connect(m_colorPanel, &ColorPanel::autoLevelsRequested, this, [this] { m_engine->requestAutoLevels(); });
    connect(m_toolsPanel, &ToolsPanel::levelsChanged, this, [this](double b, double w) {
        auto c = m_colorPanel->settings();
        c.blackPoint = b;
        c.whitePoint = w;
        m_colorPanel->setSettings(c);
        applyColorSettings(c);
    });
    connect(m_toolsPanel, &ToolsPanel::overlaysChanged, this, [this] {
        m_engine->setShowClipping(AppSettings::instance().overlays.clipping);
        applyLiveDab();
        m_view->update();
    });
    connect(m_toolsPanel, &ToolsPanel::focusRegionRequested, this, [this] {
        m_view->startPick(ImageView::PickMode::Region, tr("Drag the region used for the focus measurement"));
        disconnect(m_view, &ImageView::regionPicked, nullptr, nullptr);
        connect(m_view, &ImageView::regionPicked, this, [this](const QRect &r) {
            const double s = m_lastStats.displayScale > 0 ? m_lastStats.displayScale : 1.0;
            m_engine->setFocusRegion({int(r.x() / s), int(r.y() / s), int(r.width() / s), int(r.height() / s)});
            m_view->setFocusRegion(QRectF(r.x() / s, r.y() / s, r.width() / s, r.height() / s));
            m_toolsPanel->resetFocusPeak();
        }, Qt::SingleShotConnection);
    });
    connect(m_view, &ImageView::cursorMoved, this, &MainWindow::onCursorMoved);
    connect(m_view, &ImageView::zoomChanged, this, [this](double z) { m_statusZoom->setText(tr("Zoom %1%").arg(z * 100, 0, 'f', 0)); });
    connect(m_view, &ImageView::contextMenuRequested, this, [this](const QPoint &gp) {
        QMenu m(this);
        m.addAction(tr("Fit to window"), m_view, &ImageView::zoomFit);
        m.addAction(tr("Actual pixels (100%)"), m_view, &ImageView::zoomActual);
        m.addSeparator();
        m.addAction(tr("Capture image"), this, &MainWindow::capture);
        m.addAction(tr("Copy displayed image"), this, [this] {
            QApplication::clipboard()->setImage(m_view->renderWithOverlays(AppSettings::instance().overlays.scaleBar, false));
        });
        m.addSeparator();
        m.addAction(tr("White balance from region…"), m_colorPanel, &ColorPanel::pickWhiteBalanceRequested);
        m.exec(gp);
    });

    connect(m_scopePanel, &MicroscopePanel::calibrationChanged, this, &MainWindow::onCalibrationChanged);
    connect(m_scopePanel, &MicroscopePanel::calibrateRequested, this, &MainWindow::startCalibration);
    connect(m_scopePanel, &MicroscopePanel::shadingReferenceRequested, this, [this] {
        if (!m_engine->isLive()) {
            QMessageBox::information(this, tr("Shading correction"), tr("Start the live image first."));
            return;
        }
        m_engine->setShadingEnabled(false);
        m_engine->requestShadingReference(8);
        showMessage(tr("Acquiring shading reference…"));
    });
    connect(m_scopePanel, &MicroscopePanel::shadingEnabledChanged, this, [this](bool on) {
        m_engine->setShadingEnabled(on);
        AppSettings::instance().shadingEnabled = on;
    });
    connect(m_scopePanel, &MicroscopePanel::shadingClearRequested, this, [this] {
        if (QMessageBox::question(this, tr("Delete shading reference"),
                                  tr("Delete the shading reference for %1? You will need to acquire it again.")
                                      .arg(m_scope.currentObjective().name),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
        Objective &o = m_scope.currentObjective();
        if (!o.shadingFile.isEmpty())
            QFile::remove(o.shadingFile);
        o.shadingFile.clear();
        m_scope.save();
        m_engine->setShading(nullptr);
        loadShadingForObjective();
    });

    connect(m_capturePanel, &CapturePanel::captureRequested, this, &MainWindow::capture);
    connect(m_capturePanel, &CapturePanel::settingsChanged, this, &MainWindow::updateNextName);
    connect(m_capturePanel, &CapturePanel::timelapseToggled, this, [this](bool start) {
        const auto &c = AppSettings::instance().capture;
        if (start) {
            if (!m_engine->isLive()) {
                QMessageBox::information(this, tr("Time lapse"), tr("Start the live image first."));
                m_capturePanel->setTimelapseRunning(false, 0, c.timelapseCount);
                return;
            }
            m_timelapseDone = 0;
            m_timelapse.start(int(c.timelapseIntervalS * 1000));
            onTimelapseTick();
        } else {
            m_timelapse.stop();
            m_capturePanel->setTimelapseRunning(false, m_timelapseDone, c.timelapseCount);
            showMessage(tr("Time lapse stopped after %1 images").arg(m_timelapseDone));
        }
    });
    connect(m_capturePanel, &CapturePanel::multifocusStart, this, [this] {
        if (!m_engine->isLive()) {
            QMessageBox::information(this, tr("Multifocus"), tr("Start the live image first."));
            return;
        }
        m_engine->setLiveMode(LiveMode::Multifocus);
        m_multifocus = true;
        m_capturePanel->setMultifocusRunning(true, 0);
        m_view->setStatusText(tr("MULTIFOCUS — turn the fine focus slowly through the specimen"));
    });
    connect(m_capturePanel, &CapturePanel::multifocusFinish, this, [this] {
        m_multifocus = false;
        m_capturePanel->setMultifocusRunning(false, 0);
        m_view->setStatusText(QString());
        m_engine->finishMultifocus();
    });
    connect(m_capturePanel, &CapturePanel::multifocusCancel, this, [this] {
        m_multifocus = false;
        m_engine->setLiveMode(LiveMode::Normal);
        m_capturePanel->setMultifocusRunning(false, 0);
        m_view->setStatusText(QString());
    });
    connect(m_capturePanel, &CapturePanel::mosaicStart, this, [this] {
        if (!m_engine->isLive()) {
            QMessageBox::information(this, tr("Live image builder"), tr("Start the live image first."));
            return;
        }
        m_engine->setLiveMode(LiveMode::Mosaic);
        m_mosaic = true;
        m_capturePanel->setMosaicRunning(true, 0, true);
        m_view->setStatusText(tr("LIVE IMAGE BUILDER — move the stage slowly"));
    });
    auto endMosaic = [this] {
        m_mosaic = false;
        m_capturePanel->setMosaicRunning(false, 0, false);
        m_view->setStatusText(QString());
        m_view->setHighlightRect(QRectF());
        m_view->setTileRects({});
    };
    connect(m_capturePanel, &CapturePanel::mosaicFinish, this, [this, endMosaic] {
        endMosaic();
        m_engine->finishMosaic();
    });
    connect(m_capturePanel, &CapturePanel::mosaicCancel, this, [this, endMosaic] {
        endMosaic();
        m_engine->setLiveMode(LiveMode::Normal);
    });
    connect(m_capturePanel, &CapturePanel::mosaicAddTile, this, [this] { m_engine->mosaicAddTile(); });
    // finishes the file and reports the result; also used when the recorder
    // stopped by itself (write error or file size limit)
    auto finishRecording = [this] {
        const auto &c = AppSettings::instance().capture;
        m_recorder.stop();
        updatePreviewVisibility();
        m_recTimer.stop();
        m_view->setStatusText(QString());
        const QString err = m_recorder.lastError();
        if (!err.isEmpty()) {
            m_capturePanel->setRecording(false, tr("Recording failed"));
            QMessageBox::warning(this, tr("Video"), tr("Recording to %1 failed:\n%2")
                                                        .arg(QDir::toNativeSeparators(m_recorder.path()), err));
            return;
        }
        QString msg = tr("Saved %1 (%2 frames, %3 s, %4 MB)")
                          .arg(QFileInfo(m_recorder.path()).fileName())
                          .arg(m_recorder.frames())
                          .arg(m_recorder.frames() / double(std::max(1, c.videoFps)), 0, 'f', 1)
                          .arg(m_recorder.bytes() / 1048576.0, 0, 'f', 1);
        if (m_recorder.sizeLimitReached())
            msg = tr("Recording stopped: the video reached the maximum file size (%1 GB). ")
                      .arg(VideoRecorder::kMaxFileBytes / 1e9, 0, 'f', 1)
                  + msg;
        m_capturePanel->setRecording(false, msg);
        showMessage(msg, m_recorder.sizeLimitReached() ? 30000 : 8000);
    };
    connect(m_capturePanel, &CapturePanel::recordToggled, this, [this, finishRecording](bool start) {
        const auto &c = AppSettings::instance().capture;
        if (start) {
            if (!m_engine->isLive()) {
                QMessageBox::information(this, tr("Video"), tr("Start the live image first."));
                m_capturePanel->setRecording(false, QString());
                return;
            }
            QDir().mkpath(c.folder);
            QString sample = c.sample;
            sample.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
            const QString path = QDir(c.folder).filePath(
                QStringLiteral("%1_video_%2.avi").arg(sample, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
            QString err;
            if (!m_recorder.start(path, c.videoFps, c.videoScaleBar, &err)) {
                QMessageBox::warning(this, tr("Video"), tr("Cannot record to %1:\n%2").arg(path, err));
                m_capturePanel->setRecording(false, QString());
                return;
            }
            m_capturePanel->setRecording(true, tr("Recording…"));
            updatePreviewVisibility();
            m_view->setStatusText(tr("\u25cf REC"));
            m_recTimer.start(500);
        } else {
            finishRecording();
        }
    });
    connect(&m_recTimer, &QTimer::timeout, this, [this, finishRecording] {
        if (!m_recorder.isRecording()) { // stopped by itself: error or size limit
            finishRecording();
            return;
        }
        m_capturePanel->setRecording(true, tr("\u25cf %1 s, %2 frames, %3 MB%4")
                                               .arg(m_recorder.seconds(), 0, 'f', 0)
                                               .arg(m_recorder.frames())
                                               .arg(m_recorder.bytes() / 1048576.0, 0, 'f', 1)
                                               .arg(m_recorder.dropped() ? tr(", %1 dropped").arg(m_recorder.dropped()) : QString()));
    });
    connect(m_capturePanel, &CapturePanel::mosaicAutoAddChanged, this, [this](bool on) {
        auto o = m_engine->mosaic().options();
        o.autoAdd = on;
        m_engine->mosaic().setOptions(o);
    });
    return page;
}

void MainWindow::buildMenus()
{
    // ---- File
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(icon(Icon::Open), tr("&Open image…"), QKeySequence::Open, this, [this] {
        m_tabs->setCurrentIndex(2);
        m_process->openDialog();
    });
    file->addAction(icon(Icon::Save), tr("Save image &as…"), QKeySequence::SaveAs, m_process, &ProcessPage::saveAs);
    file->addAction(icon(Icon::Export), tr("&Export with overlays…"), QKeySequence(tr("Ctrl+E")), m_process,
                    &ProcessPage::exportWithOverlays);
    file->addAction(icon(Icon::Print), tr("&Print…"), QKeySequence::Print, m_process, &ProcessPage::print);
    file->addSeparator();
    file->addAction(icon(Icon::Folder), tr("Open image &folder"), this, [] {
        const QString d = AppSettings::instance().capture.folder;
        QDir().mkpath(d);
        QDesktopServices::openUrl(QUrl::fromLocalFile(d));
    });
    file->addSeparator();
    auto *settingsAction = file->addAction(icon(Icon::Settings), tr("&Settings…"), QKeySequence(tr("Ctrl+,")), this, [this] {
        SettingsDialog dlg(this);
        connect(&dlg, &SettingsDialog::themeChanged, this, [](const QString &t) { applyTheme(*qApp, t); });
        if (dlg.exec() == QDialog::Accepted) {
            m_capturePanel->refreshFromSettings();
            updateNextName();
        }
    });
    // macOS moves these two into the application menu; the roles tell Qt which.
    settingsAction->setMenuRole(QAction::PreferencesRole);
    file->addSeparator();
    auto *quitAction = file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);
    quitAction->setMenuRole(QAction::QuitRole);

    // ---- Acquire
    QMenu *acq = menuBar()->addMenu(tr("&Acquire"));
    auto *live = acq->addAction(tr("&Live image"), QKeySequence(Qt::Key_F5), this, [this] {
        if (m_engine->isLive())
            m_engine->stopLive();
        else {
            QString err;
            if (!m_engine->camera())
                m_cameraPanel->connectCamera(0);
            if (m_engine->camera() && !m_engine->startLive(err))
                showMessage(err, 8000);
        }
    });
    Q_UNUSED(live);
    acq->addAction(tr("&Freeze image"), QKeySequence(Qt::Key_F6), this, [this] {
        m_engine->setFrozen(!m_engine->isFrozen());
        m_view->setStatusText(m_engine->isFrozen() ? tr("FROZEN") : QString());
    });
    acq->addAction(icon(Icon::Capture), tr("&Capture image"), QKeySequence(Qt::Key_F9), this, &MainWindow::capture);
    acq->addSeparator();
    acq->addAction(icon(Icon::Wand), tr("Auto &white balance"), QKeySequence(Qt::Key_F7), this,
                   [this] { m_engine->requestWhiteBalance(); });
    acq->addAction(icon(Icon::Wand), tr("Auto &exposure once"), QKeySequence(Qt::Key_F8), this,
                   [this] { m_engine->requestAutoExposureOnce(); });
    acq->addAction(icon(Icon::Wand), tr("Auto &levels"), this, [this] { m_engine->requestAutoLevels(); });
    acq->addSeparator();
    QMenu *obj = acq->addMenu(tr("&Objective"));
    obj->setIcon(icon(Icon::Objective));
    for (int i = 0; i < 9; ++i) {
        auto *a = obj->addAction(QString(), QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)), this, [this, i] {
            if (i < m_scope.objectives.size()) {
                m_scope.current = i;
                m_scope.save();
                m_scopePanel->refresh();
                onCalibrationChanged();
                showMessage(tr("Objective: %1").arg(m_scope.objectiveLabel(m_scope.currentObjective())));
            }
        });
        connect(obj, &QMenu::aboutToShow, a, [this, a, i] {
            a->setVisible(i < m_scope.objectives.size());
            if (i < m_scope.objectives.size()) {
                a->setText(m_scope.objectiveLabel(m_scope.objectives[i]));
                a->setCheckable(true);
                a->setChecked(i == m_scope.current);
            }
        });
    }

    // ---- View
    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(icon(Icon::Acquire), tr("&Acquire"), QKeySequence(tr("Alt+1")), this, [this] { m_tabs->setCurrentIndex(0); });
    view->addAction(icon(Icon::Browse), tr("&Browse"), QKeySequence(tr("Alt+2")), this, [this] { m_tabs->setCurrentIndex(1); });
    view->addAction(icon(Icon::Process), tr("&Process"), QKeySequence(tr("Alt+3")), this, [this] { m_tabs->setCurrentIndex(2); });
    view->addSeparator();
    view->addAction(icon(Icon::ZoomFit), tr("Zoom to &fit"), QKeySequence(tr("Ctrl+0")), m_view, &ImageView::zoomFit);
    view->addAction(icon(Icon::ZoomActual), tr("Actual &pixels"), QKeySequence(tr("Ctrl+Alt+0")), m_view, &ImageView::zoomActual);
    view->addAction(icon(Icon::ZoomIn), tr("Zoom &in"), QKeySequence::ZoomIn, m_view, &ImageView::zoomIn);
    view->addAction(icon(Icon::ZoomOut), tr("Zoom &out"), QKeySequence::ZoomOut, m_view, &ImageView::zoomOut);
    view->addSeparator();
    view->addAction(icon(Icon::Fullscreen), tr("F&ull screen"), QKeySequence(Qt::Key_F11), this, [this] {
        isFullScreen() ? showNormal() : showFullScreen();
    });

    // ---- Process
    QMenu *proc = menuBar()->addMenu(tr("&Process"));
    proc->addAction(icon(Icon::Multifocus), tr("&Multifocus from files…"), this, [this] {
        m_tabs->setCurrentIndex(2);
        m_process->multifocusFromFiles();
    });
    proc->addAction(icon(Icon::Stitch), tr("&Stitch images from files…"), this, [this] {
        m_tabs->setCurrentIndex(2);
        m_process->stitchFromFiles();
    });
    proc->addAction(icon(Icon::Calibrate), tr("Set &pixel size…"), m_process, &ProcessPage::setCalibration);
    proc->addSeparator();
    proc->addAction(icon(Icon::Compare), tr("&Compare two images…"), QKeySequence(tr("Ctrl+K")), this, [this] {
        auto *w = new CompareWindow(this);
        if (m_process->hasImage() && !m_process->currentPath().isEmpty())
            w->openLeft(m_process->currentPath());
        w->show();
    });

    // ---- Tools
    QMenu *tools = menuBar()->addMenu(tr("&Tools"));
    tools->addAction(icon(Icon::Calibrate), tr("&Calibrate objective with stage micrometer…"), this,
                     &MainWindow::startCalibration);
    if (cameraAccessSetupAvailable())
        tools->addAction(icon(Icon::Chip), cameraAccessSetupLabel(), this, [this] {
            const QString r = setUpCameraAccess(this);
            if (!r.isEmpty())
                showMessage(r, 6000);
            m_cameraPanel->refreshCameras();
        });
    tools->addAction(icon(Icon::Refresh), tr("&Search cameras"), m_cameraPanel, &CameraPanel::refreshCameras);

    // ---- Help
    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(icon(Icon::Help), tr("&Keyboard shortcuts"), this, [this] {
        // Qt maps Ctrl to Command on macOS; name the key the user's keyboard has.
#ifdef Q_OS_MACOS
        const QString ctrl = QStringLiteral("Cmd");
#else
        const QString ctrl = QStringLiteral("Ctrl");
#endif
        QMessageBox box(QMessageBox::NoIcon, tr("Keyboard shortcuts"),
                        tr("<b>Camera</b><br>"
                           "F5 — live on/off · F6 — freeze<br>"
                           "F7 — auto white balance · F8 — auto exposure once<br>"
                           "F9 or Space — capture image<br>"
                           "%1+1, %1+2, … — select objective<br><br>"
                           "<b>Workspaces</b><br>"
                           "Alt+1 / Alt+2 / Alt+3 — Acquire / Browse / Process · F11 — full screen<br><br>"
                           "<b>Image</b><br>"
                           "Mouse wheel over the image — zoom<br>"
                           "Double click — fit / 100%%  ·  0 / 1 / 2 — fit / 100%% / 200%%<br>"
                           "%1+drag or middle drag — pan<br>"
                           "%1+0 — fit · %1++ / %1+- — zoom in / out<br><br>"
                           "<b>Annotations</b><br>"
                           "Del — delete selected · %1+Z / %1+Y — undo / redo<br><br>"
                           "<b>Side panels</b><br>"
                           "The mouse wheel scrolls the panel. It never changes a setting: "
                           "drag a slider, type in the box, or use the arrow keys.")
                            .arg(ctrl),
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::RichText);
        box.exec();
    });
    auto *aboutAction = help->addAction(icon(Icon::Info), tr("&About DM Imaging"), this, [this] {
        QString cam;
        if (m_engine->camera())
            cam = QString::fromStdString(m_engine->camera()->info().name);
        AboutDialog(cam, this).exec();
    });
    aboutAction->setMenuRole(QAction::AboutRole); // macOS: into the application menu
}

void MainWindow::startup()
{
    m_cameraPanel->autoConnect();
    if (!m_engine->camera()) {
        const auto cams = m_cameraPanel->cameras();
        const bool anyLeica = std::any_of(cams.begin(), cams.end(), [](const CameraInfo &c) { return c.backend == "Leica USB"; });
        if (!anyLeica)
            showMessage(tr("No Leica camera found. Check the USB cable, or install the driver (Tools menu)."), 15000);
        // keep looking for the camera (plugged in later, or power cycled)
        m_lostCameraId.clear();
        m_view->setPlaceholder(tr("Waiting for the Leica camera…\n\nConnect the camera (USB 3.0). If it is connected, unplug it for "
                                  "5 seconds and plug it back in.\nThe live image starts automatically."));
        m_reconnect.start();
    }
    onCameraChanged();
}

void MainWindow::applyLiveDab()
{
    // same stain colours and threshold as the IHC analysis in Process
    const auto &S = AppSettings::instance();
    StainOptions so;
    so.dabThreshold = S.ihc.dabThreshold;
    for (int c = 0; c < 3; ++c) {
        so.vectors.h[c] = S.ihc.h[c];
        so.vectors.dab[c] = S.ihc.dab[c];
    }
    m_engine->setLiveDab(S.overlays.liveDab, so);
}

void MainWindow::updatePreviewVisibility()
{
    // the live image is only rendered at full rate when someone can see it (or a video records it)
    m_engine->setPreviewVisible((m_stack->currentIndex() == 0 && !isMinimized()) || m_recorder.isRecording());
}

void MainWindow::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange)
        updatePreviewVisibility();
}

void MainWindow::setWorkspace(int index)
{
    m_stack->setCurrentIndex(index);
    updatePreviewVisibility();
    if (index == 0)
        applyLiveDab(); // stain settings may have changed in Process
    if (index == 1)
        m_browse->refresh();
}

void MainWindow::applyColorSettings(const ColorSettings &c)
{
    m_engine->setColorSettings(c);
    AppSettings::instance().color = c;
    m_toolsPanel->setLevels(c.blackPoint, c.whitePoint);
}

void MainWindow::onFrame(const QImage &img, const LiveStats &stats)
{
    static const bool profile = qEnvironmentVariableIsSet("DMI_PROFILE");
    QElapsedTimer timer;
    if (profile)
        timer.start();
    m_lastStats = stats;
    m_view->setSensorScale(stats.displayScale);
    m_view->setImage(img);
    m_view->setOverlayImage(stats.dabOverlay);
    if (m_recorder.isRecording())
        m_recorder.push(img, m_view->umPerPixel());
    // choose the live preview resolution from the effective zoom (sensor pixels)
    if (stats.width > 0 && stats.height > 0) {
        // effective zoom in physical screen pixels per sensor pixel (high-DPI aware)
        const double dpr = m_view->devicePixelRatioF();
        const double eff = dpr * (m_view->isFit()
                                      ? std::min(double(m_view->width()) / stats.width, double(m_view->height()) / stats.height)
                                      : m_view->zoom() * stats.displayScale);
        static bool half = false;
        if (!half && eff < 0.6)
            half = true;
        else if (half && eff > 0.75)
            half = false;
        m_engine->setPreviewHalf(half);
    }
    const double um = m_scope.umPerPixel();
    m_view->setUmPerPixel(stats.displayScale > 0 ? um / stats.displayScale : um);
    m_toolsPanel->setStats(stats, um);
    m_view->setFocusValue(stats.focus, m_toolsPanel->focusPeak());
    m_statusFps->setText(tr("%1 fps").arg(stats.fps, 0, 'f', 1));
    if (Camera *cam = m_engine->camera())
        m_statusExposure->setText(tr("Exp. %1 ms · Gain %2×").arg(cam->exposure(), 0, 'g', 4).arg(cam->gain(), 0, 'f', 2));
    // let the engine render the next frame once this one has been painted
    QMetaObject::invokeMethod(this, [this] { m_engine->frameConsumed(); }, Qt::QueuedConnection);
    if (profile) {
        static double acc = 0;
        static int n = 0;
        acc += timer.nsecsElapsed() / 1e6;
        if (++n == 50) {
            qInfo("onFrame avg %.2f ms (display %.1f fps)", acc / n, stats.displayFps);
            acc = 0;
            n = 0;
        }
    }
}

void MainWindow::onCursorMoved(const QPoint &p, bool inside)
{
    if (!inside) {
        m_statusCursor->clear();
        m_toolsPanel->setPixelInfo(QString());
        return;
    }
    const QImage &img = m_view->image();
    const QRgb c = img.pixel(p);
    const double um = m_view->umPerPixel();
    // report sensor pixel coordinates (the live preview may be half resolution)
    const double s = m_tabs->currentIndex() == 0 && m_lastStats.displayScale > 0 ? m_lastStats.displayScale : 1.0;
    QString t = tr("x %1, y %2   RGB %3 %4 %5").arg(int(p.x() / s)).arg(int(p.y() / s)).arg(qRed(c)).arg(qGreen(c)).arg(qBlue(c));
    if (um > 0)
        t += tr("   (%1, %2 µm)").arg(p.x() * um, 0, 'f', 1).arg(p.y() * um, 0, 'f', 1);
    m_statusCursor->setText(t);
    m_toolsPanel->setPixelInfo(t);
}

void MainWindow::onCameraChanged()
{
    Camera *cam = m_engine->camera();
    if (!cam) {
        m_statusCamera->setText(tr("No camera"));
        m_capturePanel->setShotModes({});
        m_view->clear();
        updateTitle();
        return;
    }
    m_statusCamera->setText(QString::fromStdString(cam->info().name));
    QStringList modes;
    for (const auto &m : cam->shotModes())
        modes << QString::fromStdString(m.name);
    m_capturePanel->setShotModes(modes);
    // restore saved acquisition settings
    auto &S = AppSettings::instance();
    AutoExposureSettings ae = m_engine->autoExposure();
    ae.enabled = S.autoExposure;
    ae.target = S.aeTarget;
    m_engine->setAutoExposure(ae);
    m_engine->setShowClipping(S.overlays.clipping);
    applyLiveDab();
    if (S.shadingEnabled)
        m_scopePanel->setShadingEnabled(true);
    // first use: white balance is still neutral -> balance on the live image
    const ColorSettings cs = m_engine->colorSettings();
    if (cam->deliversRaw() && cs.wbRed == 1.0 && cs.wbGreen == 1.0 && cs.wbBlue == 1.0) {
        QTimer::singleShot(1500, this, [this] {
            if (m_engine->isLive()) {
                m_engine->requestWhiteBalance();
                showMessage(tr("Automatic white balance applied (Colour panel to adjust)"), 6000);
            }
        });
    }
    updateTitle();
}

void MainWindow::storeObjectiveSettings(int index)
{
    if (index < 0 || index >= m_scope.objectives.size())
        return;
    Objective &o = m_scope.objectives[index];
    Camera *cam = m_engine->camera();
    o.exposureMs = cam ? cam->exposure() : AppSettings::instance().exposureMs;
    o.gain = cam ? cam->gain() : AppSettings::instance().gain;
    const ColorSettings cs = m_engine->colorSettings();
    o.wbRed = cs.wbRed;
    o.wbGreen = cs.wbGreen;
    o.wbBlue = cs.wbBlue;
}

void MainWindow::restoreObjectiveSettings(int index)
{
    if (index < 0 || index >= m_scope.objectives.size())
        return;
    const Objective &o = m_scope.objectives[index];
    if (o.exposureMs <= 0)
        return; // nothing stored yet for this objective
    if (Camera *cam = m_engine->camera()) {
        if (!m_engine->autoExposure().enabled)
            cam->setExposure(o.exposureMs);
        cam->setGain(o.gain);
        m_cameraPanel->setExposureDisplay(cam->exposure(), cam->gain());
    }
    if (o.wbRed > 0 && o.wbGreen > 0 && o.wbBlue > 0)
        m_colorPanel->setWhiteBalance(o.wbRed, o.wbGreen, o.wbBlue);
    showMessage(tr("%1: exposure %2 ms and white balance restored").arg(m_scope.objectiveLabel(o)).arg(o.exposureMs, 0, 'g', 4));
}

void MainWindow::onCalibrationChanged()
{
    // objective switched: keep per-objective camera settings
    if (m_prevObjective != m_scope.current) {
        if (m_prevObjective >= 0 && m_scope.rememberSettings) {
            if (m_objectiveFromCapture) {
                // the settings just used belong to the objective chosen in the capture dialog
                storeObjectiveSettings(m_scope.current);
            } else {
                storeObjectiveSettings(m_prevObjective);
                restoreObjectiveSettings(m_scope.current);
            }
            m_scope.save();
        }
        m_prevObjective = m_scope.current;
    }
    m_view->setUmPerPixel(m_scope.umPerPixel() / std::max(0.01, m_lastStats.displayScale));
    loadShadingForObjective();
    updateNextName();
    updateTitle();
}

void MainWindow::loadShadingForObjective()
{
    const Objective &o = m_scope.currentObjective();
    std::shared_ptr<ShadingCorrection> sc;
    if (!o.shadingFile.isEmpty())
        sc = ShadingCorrection::load(o.shadingFile.toStdString());
    m_engine->setShading(sc);
    if (sc)
        m_scopePanel->setShadingStatus(tr("Reference for %1 (max. correction %2×)")
                                           .arg(m_scope.objectiveLabel(o))
                                           .arg(sc->maxGain(), 0, 'f', 2),
                                       true);
    else
        m_scopePanel->setShadingStatus(tr("No reference for %1").arg(m_scope.objectiveLabel(o)), false);
    m_engine->setShadingEnabled(sc && m_scopePanel->shadingEnabled());
}

void MainWindow::startCalibration()
{
    if (m_view->image().isNull()) {
        QMessageBox::information(this, tr("Calibration"), tr("Start the live image with a stage micrometer in focus first."));
        return;
    }
    m_tabs->setCurrentIndex(0);
    m_calibPoints.clear();
    m_view->startPick(ImageView::PickMode::Point, tr("Calibration: click the FIRST mark of a known distance"));
    disconnect(m_view, &ImageView::pointPicked, nullptr, nullptr);
    connect(m_view, &ImageView::pointPicked, this, [this](const QPoint &p) {
        m_calibPoints.append(p);
        if (m_calibPoints.size() == 1) {
            m_view->startPick(ImageView::PickMode::Point, tr("Calibration: click the SECOND mark"));
            return;
        }
        disconnect(m_view, &ImageView::pointPicked, nullptr, nullptr);
        const double dx = m_calibPoints[1].x() - m_calibPoints[0].x();
        const double dy = m_calibPoints[1].y() - m_calibPoints[0].y();
        const double px = std::hypot(dx, dy) / std::max(0.01, m_lastStats.displayScale);
        if (px < 10) {
            QMessageBox::warning(this, tr("Calibration"),
                                 tr("The points are too close together. Click two marks at least 100 µm apart "
                                    "(further apart is more accurate)."));
            return;
        }
        bool ok = false;
        const double um = QInputDialog::getDouble(this, tr("Calibration"),
                                                  tr("Distance between the marks (%1 pixels) in µm:").arg(px, 0, 'f', 1),
                                                  100.0, 0.01, 100000.0, 3, &ok);
        if (!ok)
            return;
        Objective &o = m_scope.currentObjective();
        o.calibratedUmPerPixel = um / px;
        m_scope.save();
        m_scopePanel->refresh();
        onCalibrationChanged();
        QMessageBox::information(this, tr("Calibration"),
                                 tr("%1 calibrated: %2 µm/pixel (nominal %3 µm/pixel).")
                                     .arg(m_scope.objectiveLabel(o))
                                     .arg(o.calibratedUmPerPixel, 0, 'g', 5)
                                     .arg(m_scope.nominalUmPerPixel(o), 0, 'g', 5));
    });
}

void MainWindow::capture()
{
    if (m_capturing || m_engine->isBusy()) {
        showMessage(tr("A capture is already in progress"));
        return;
    }
    if (!m_engine->camera()) {
        QMessageBox::information(this, tr("Capture"), tr("No camera connected."));
        return;
    }
    const auto &c = AppSettings::instance().capture;
    m_capturing = true;
    m_cameraPanel->setBusy(true);
    if (c.shotMode >= 0 && !m_engine->camera()->shotModes().empty()) {
        m_capturePanel->setBusy(true, tr("Pixel shift capture…"));
        m_engine->captureShots(c.shotMode);
    } else {
        m_capturePanel->setBusy(true);
        m_engine->capture(c.averageFrames);
    }
}

ImageMetadata MainWindow::currentMetadata(const CaptureResult &r) const
{
    const auto &S = AppSettings::instance();
    ImageMetadata m;
    m.softwareVersion = QCoreApplication::applicationVersion() + QStringLiteral(" (" DMI_GIT_HASH ")");
    m.acquired = QDateTime::currentDateTime();
    if (Camera *cam = m_engine->camera()) {
        const CameraInfo ci = cam->info();
        m.camera = QString::fromStdString(ci.name);
        m.cameraSerial = QString::fromStdString(ci.serial);
        for (const auto &[k, v] : cam->details())
            if (k == "Sensor")
                m.sensor = QString::fromStdString(v);
    }
    m.microscope = m_scope.microscopeName;
    const Objective &o = m_scope.currentObjective();
    m.objective = m_scope.objectiveLabel(o);
    m.magnification = o.magnification;
    m.numericalAperture = o.na;
    m.adapterFactor = m_scope.adapterFactor;
    m.umPerPixel = m_scope.umPerPixel() / std::max(1, r.upscale);
    m.exposureMs = r.exposureMs;
    m.gain = r.gain;
    m.averagedFrames = r.averagedFrames;
    m.captureMode = QString::fromStdString(r.kind);
    const ColorSettings cs = m_engine->colorSettings();
    m.wbRed = cs.wbRed;
    m.wbGreen = cs.wbGreen;
    m.wbBlue = cs.wbBlue;
    m.gamma = cs.gamma;
    m.saturation = cs.saturation;
    m.blackLevel = cs.blackLevel;
    if (!cs.grayscale && (cs.filterTemperature != 0.0 || cs.filterTint != 0.0))
        m.lightFilter = QStringLiteral("temperature %1, tint %2")
                            .arg(cs.filterTemperature, 0, 'f', 0)
                            .arg(cs.filterTint, 0, 'f', 0);
    m.shadingCorrected = m_engine->shadingEnabled() && m_engine->shading();
    {
        static const std::array<double, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
        m.colorCorrection = !cs.colorCorrection            ? QStringLiteral("off")
                            : cs.cameraMatrix == identity ? QStringLiteral("none (camera has no calibration)")
                                                          : QStringLiteral("camera matrix (%1)").arg(QString::fromStdString(
                                                                m_engine->camera() ? m_engine->camera()->colorMatrixName()
                                                                                   : std::string()));
    }
    m.sample = S.capture.sample;
    m.operatorName = S.capture.operatorName;
    return m;
}

void MainWindow::onCaptureFinished(std::shared_ptr<CaptureResult> r)
{
    m_capturing = false;
    m_capturePanel->setBusy(false);
    m_cameraPanel->setBusy(false);
    if (!r || r->rendered16.empty()) {
        showMessage(tr("Capture produced no image. Try again; if it repeats, stop and restart the live image (F5)."), 8000);
        return;
    }
    auto &S = AppSettings::instance();
    QDir().mkpath(S.capture.folder);
    const QString mode = QString::fromStdString(r->kind);
    // names on disk or of images still being written must not be reused
    auto taken = [this](const QString &p) { return QFileInfo::exists(p) || m_pendingSaves.contains(p); };
    auto withSuffix = [&](const QString &p) {
        if (!taken(p))
            return p;
        const QFileInfo fi(p);
        for (int n = 2;; ++n) {
            const QString q = fi.dir().filePath(QStringLiteral("%1_%2.%3").arg(fi.completeBaseName()).arg(n).arg(fi.suffix()));
            if (!taken(q))
                return q;
        }
    };
    auto suggested = [&](int objective) {
        const QString obj = shortObjective(m_scope.objectives.value(objective, m_scope.currentObjective()));
        QString p = S.nextFileName(obj, mode);
        // advance the counter past existing files; a template without {counter}
        // (the name does not change) gets a _2, _3, ... suffix instead
        for (int i = 0; i < 100000 && taken(p); ++i) {
            S.capture.counter++;
            const QString next = S.nextFileName(obj, mode);
            if (next == p)
                break;
            p = next;
        }
        return withSuffix(p);
    };
    QString path = suggested(m_scope.current);
    QString notes;

    // ask for the objective and the image name (manual microscope: the turret is not coded)
    if (S.capture.promptAfterCapture && !m_timelapse.isActive()) {
        const QString info = tr("%1 × %2 px  ·  %3  ·  exposure %4 ms")
                                 .arg(r->rendered16.width)
                                 .arg(r->rendered16.height)
                                 .arg(mode)
                                 .arg(r->exposureMs, 0, 'g', 4);
        CaptureDialog dlg(toQImage8(r->rendered8), m_scope, m_scope.current,
                          [&](int obj) { return QFileInfo(suggested(obj)).completeBaseName(); }, info, this);
        if (dlg.exec() != QDialog::Accepted) {
            showMessage(tr("Image discarded"), 4000);
            return;
        }
        if (dlg.objectiveIndex() >= 0 && dlg.objectiveIndex() != m_scope.current) {
            m_scope.current = dlg.objectiveIndex();
            m_scope.save();
            m_scopePanel->refresh();
            m_objectiveFromCapture = true;
            onCalibrationChanged();
            m_objectiveFromCapture = false;
        }
        notes = dlg.notes();
        const QString ext = QLatin1Char('.') + extensionFor(S.capture.save.format);
        path = QDir(S.capture.folder).filePath(dlg.imageName() + ext);
        for (int n = 2; QFileInfo::exists(path) || m_pendingSaves.contains(path); ++n)
            path = QDir(S.capture.folder).filePath(QStringLiteral("%1_%2").arg(dlg.imageName()).arg(n) + ext);
    }
    S.capture.counter++;
    S.save();
    updateNextName();

    ImageMetadata meta = currentMetadata(*r);
    meta.notes = notes;
    if (S.capture.promptAfterCapture)
        meta.sample = QFileInfo(path).completeBaseName();
    const SaveOptions opt = S.capture.save;
    const bool burn = S.capture.burnScaleBar;
    const OverlaySettings ov = S.overlays;
    const QImage thumbSrc = toQImage8(r->rendered8);
    showMessage(tr("Saving %1…").arg(QFileInfo(path).fileName()), 0);
    m_pendingSaves.insert(path);
    QtConcurrent::run([r, path, meta, opt, burn, ov]() -> QString {
      try {
        QString err;
        bool ok;
        if (burn && meta.umPerPixel > 0) {
            QImage q(r->rendered16.width, r->rendered16.height, QImage::Format_RGBX64);
            for (int y = 0; y < q.height(); ++y) {
                auto *d = reinterpret_cast<quint16 *>(q.scanLine(y));
                const uint16_t *s = r->rendered16.row(y);
                for (int x = 0; x < q.width(); ++x) {
                    d[x * 4] = s[x * 3];
                    d[x * 4 + 1] = s[x * 3 + 1];
                    d[x * 4 + 2] = s[x * 3 + 2];
                    d[x * 4 + 3] = 0xFFFF;
                }
            }
            ok = saveImage(path, burnScaleBar(q, meta.umPerPixel, ov), meta, opt, &err);
        } else {
            ok = saveImage(path, r->rendered16, meta, opt, &err);
        }
        return ok ? QString() : (err.isEmpty() ? QObject::tr("unknown error") : err);
      } catch (const std::exception &e) {
        return QObject::tr("internal error: %1").arg(QString::fromUtf8(e.what()));
      } catch (...) {
        return QObject::tr("internal error");
      }
    }).then(this, [this, path, thumbSrc, r, meta](const QString &err) {
        m_pendingSaves.remove(path);
        if (!err.isEmpty()) {
            QMessageBox::warning(this, tr("Save image"), tr("Could not save %1:\n%2").arg(path, err));
            return;
        }
        m_gallery->addImage(path, thumbSrc);
        showMessage(tr("Saved %1 (%2 × %3)").arg(QDir::toNativeSeparators(path)).arg(r->rendered16.width).arg(r->rendered16.height), 6000);
        if (AppSettings::instance().capture.openInProcess && !m_timelapse.isActive()) {
            m_process->openImage(r->rendered16, meta, path);
            m_tabs->setCurrentIndex(2);
        }
    });
}

void MainWindow::onTimelapseTick()
{
    const auto &c = AppSettings::instance().capture;
    if (m_timelapseDone >= c.timelapseCount) {
        m_timelapse.stop();
        m_capturePanel->setTimelapseRunning(false, m_timelapseDone, c.timelapseCount);
        showMessage(tr("Time lapse finished (%1 images)").arg(m_timelapseDone), 8000);
        return;
    }
    if (m_capturing || m_engine->isBusy())
        return; // previous capture still running; skip this slot
    ++m_timelapseDone;
    capture();
    m_capturePanel->setTimelapseRunning(true, m_timelapseDone, c.timelapseCount);
}

void MainWindow::onCameraLost(const QString &reason)
{
    qWarning("camera error: %s", qPrintable(reason));
    if (m_reconnect.isActive())
        return;
    if (Camera *cam = m_engine->camera())
        m_lostCameraId = QString::fromStdString(cam->info().id);
    m_timelapse.stop();
    m_engine->closeCamera();
    m_cameraPanel->syncFromCamera();
    onCameraChanged();
    m_view->setStatusText(tr("CAMERA DISCONNECTED — reconnecting automatically…"));
    m_view->setPlaceholder(tr("Camera disconnected.\n\nReconnecting automatically. If this persists, unplug the camera's "
                              "USB cable for 5 seconds and plug it back in."));
    showMessage(tr("Camera connection lost: %1").arg(reason), 0);
    m_reconnect.start();
}

void MainWindow::tryReconnect()
{
    if (m_engine->camera()) {
        // the user connected a camera manually in the meantime
        m_reconnect.stop();
        m_view->setStatusText(QString());
        return;
    }
    m_cameraPanel->refreshCameras();
    const auto cams = m_cameraPanel->cameras();
    for (int i = 0; i < int(cams.size()); ++i) {
        const auto &c = cams[size_t(i)];
        if (QString::fromStdString(c.id) == m_lostCameraId || (m_lostCameraId.isEmpty() && c.backend == "Leica USB")) {
            if (m_cameraPanel->connectCamera(i)) {
                QString err;
                if (m_engine->startLive(err)) {
                    m_reconnect.stop();
                    m_view->setStatusText(QString());
                    showMessage(tr("Camera reconnected"), 5000);
                    qInfo("camera reconnected");
                } else {
                    m_engine->closeCamera();
                }
            }
            return;
        }
    }
}

void MainWindow::showMessage(const QString &text, int timeoutMs)
{
    statusBar()->showMessage(text, timeoutMs);
}

void MainWindow::updateTitle()
{
    QString t = QStringLiteral("DM Imaging");
    if (Camera *cam = m_engine->camera())
        t += QStringLiteral(" — ") + QString::fromStdString(cam->info().name);
    t += QStringLiteral(" — ") + m_scope.objectiveLabel(m_scope.currentObjective());
    setWindowTitle(t);
}

void MainWindow::updateNextName()
{
    m_capturePanel->updateNextName(QFileInfo(AppSettings::instance().nextFileName(shortObjective(m_scope.currentObjective()),
                                                                                  QStringLiteral("single")))
                                       .fileName());
}

bool MainWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_view && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Space && !ke->isAutoRepeat()) {
            capture();
            return true;
        }
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (m_capturing || m_engine->isBusy()) {
        if (QMessageBox::question(this, tr("Quit"), tr("A capture is in progress. Quit anyway?")) != QMessageBox::Yes) {
            e->ignore();
            return;
        }
    }
    m_timelapse.stop();
    m_recorder.stop();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_engine->waitForJobs();                  // finish captures in progress
    m_engine->stopLive();                     // no more frames queued while waiting below
    QThreadPool::globalInstance()->waitForDone(); // finish image saves
    QApplication::restoreOverrideCursor();
    QSettings qs;
    qs.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    AppSettings::instance().save();
    if (m_scope.rememberSettings && m_engine->camera())
        storeObjectiveSettings(m_scope.current);
    m_scope.save();
    m_engine->closeCamera();
    e->accept();
}

} // namespace lm
