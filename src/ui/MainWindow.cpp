#include "MainWindow.h"

#include "app/AppSettings.h"
#include "imaging/ColorPipeline.h"
#include "io/ImageIO.h"
#include "io/LifFile.h"
#include "ui/BrowsePage.h"
#include "ui/CameraPanel.h"
#include "ui/CalibrationRepairDialog.h"
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
#include <QGuiApplication>
#include <QScreen>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileDialog>
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
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QButtonGroup>
#include <QActionGroup>
#include <QMenuBar>
#include <QPainter>
#include <QMessageBox>
#include <QScrollArea>
#include <QActionGroup>
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
// interface font is larger on macOS and Linux than on Windows). Scaled with
// the interface size, or a larger font would just clip.
constexpr int kPanelWidthAt100 = 340;
int panelWidth()
{
    return px(kPanelWidthAt100);
}

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
    // The minimum grows with the interface size, but never past the screen: at
    // 200% an unclamped px(1100) asks for a 2200 px window, and everything to
    // the right of it is simply unreachable.
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QSize avail = screen->availableGeometry().size();
        setMinimumSize(std::min(px(1100), int(avail.width() * 0.95)), std::min(px(700), int(avail.height() * 0.95)));
    } else {
        setMinimumSize(px(1100), px(700));
    }

    // ---- top bar with workflow tabs
    auto *central = new QWidget(this);
    auto *cl = new QVBoxLayout(central);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    auto *top = new QWidget(central);
    top->setObjectName(QStringLiteral("TopBar"));
    auto *tl = new QHBoxLayout(top);
    tl->setContentsMargins(0, 0, px(8), 0);
    auto *logo = new QLabel(top);
    logo->setPixmap(QIcon(QStringLiteral(":/icons/app.png")).pixmap(lm::iconSize(20)));
    logo->setContentsMargins(px(12), 0, 0, 0);
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
    m_tabs->setIconSize(lm::iconSize(16));
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
    m_statusCamera->setToolTip(tr("The connected camera"));
    m_statusFps = new QLabel;
    m_statusFps->setToolTip(tr("Frames per second delivered by the camera"));
    m_statusExposure = new QLabel;
    m_statusCursor = new QLabel;
    m_statusCursor->setToolTip(tr("Position and colour under the pointer"));
    m_statusZoom = new QLabel;
    // a dot that says at a glance whether the camera is there
    m_statusLed = new QLabel;
    m_statusLed->setToolTip(m_statusCamera->toolTip());
    setCameraLed(CameraState::None);
    statusBar()->addPermanentWidget(m_statusCursor);
    statusBar()->addPermanentWidget(m_statusZoom);
    statusBar()->addPermanentWidget(m_statusExposure);
    statusBar()->addPermanentWidget(m_statusFps);
    statusBar()->addPermanentWidget(m_statusLed);
    statusBar()->addPermanentWidget(m_statusCamera);

    buildMenus();

    connect(m_tabs, &QTabBar::currentChanged, this, &MainWindow::setWorkspace);
    connect(m_engine, &AcquisitionEngine::frameReady, this, &MainWindow::onFrame, Qt::QueuedConnection);
    connect(m_engine, &AcquisitionEngine::captureFinished, this, &MainWindow::onCaptureFinished, Qt::QueuedConnection);
    connect(m_engine, &AcquisitionEngine::captureFailed, this, [this](const QString &m) {
        m_capturing = false;
        m_capturePanel->setBusy(false);
        m_cameraPanel->setBusy(false);
        updateCaptureState();
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
        if (m_engine->camera())
            setCameraLed(live ? CameraState::Live : CameraState::Ready);
        if (!live)
            m_statusFps->clear(); // no frames: the last rate no longer applies
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
    connect(m_gallery, &GalleryWidget::referenceRequested, this, &MainWindow::showReference);
    connect(m_gallery, &GalleryWidget::renamed, this, [this](const QString &from, const QString &to) {
        m_process->fileRenamed(from, to);
        if (m_referencePath == from)
            m_referencePath = to;
        showMessage(tr("Renamed to %1").arg(QFileInfo(to).fileName()), 5000);
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
    m_acquireSplitter = split;

    // left: camera, microscope, capture
    auto *left = new QWidget;
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(0);
    m_cameraPanel = new CameraPanel(m_engine, left);
    m_scopePanel = new MicroscopePanel(&m_scope, left);
    m_capturePanel = new CapturePanel(left);
    // Capturing is what the page is for, so it comes first, where the eye starts;
    // then the camera and the microscope, which are set up once per session
    ll->addWidget(m_capturePanel);
    ll->addWidget(m_cameraPanel);
    ll->addWidget(m_scopePanel);
    ll->addStretch();
    m_leftPanel = panelScroll(left, panelWidth());
    split->addWidget(m_leftPanel);

    // centre: the live image, with the captured images either under it (reel)
    // or beside it (list); see setGalleryVertical()
    auto *centre = new QSplitter(Qt::Vertical, split);
    m_centreSplitter = centre;
    m_view = new ImageView(centre);
    m_view->setPlaceholder(tr("No camera connected\n\nConnect the camera in the Camera panel (or use the simulator)."));
    m_view->installEventFilter(this);
    // the strip with a header that names it and switches its layout
    auto *galleryBox = new QWidget(centre);
    auto *gbl = new QVBoxLayout(galleryBox);
    gbl->setContentsMargins(0, 0, 0, 0);
    gbl->setSpacing(0);
    auto *gh = new QWidget(galleryBox);
    auto *ghl = new QHBoxLayout(gh);
    ghl->setContentsMargins(px(8), px(3), px(4), px(3));
    ghl->setSpacing(px(2));
    auto *ghTitle = new QLabel(tr("Captured images"), gh);
    ghTitle->setObjectName(QStringLiteral("ControlLabel"));
    ghl->addWidget(ghTitle);
    ghl->addStretch();
    auto *layoutGroup = new QButtonGroup(gh); // exactly one layout is on
    layoutGroup->setExclusive(true);
    auto layoutButton = [&](Icon ic, const QString &text, const QString &tip, int mode) {
        auto *b = new QToolButton(gh);
        b->setIcon(icon(ic));
        b->setText(text);
        b->setToolTip(tip);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        // on being checked, not on clicked: keyboard and accessibility "press"
        // toggle a checkable button without a click
        connect(b, &QToolButton::toggled, this, [this, mode](bool on) {
            if (on)
                setGalleryMode(mode);
        });
        layoutGroup->addButton(b);
        ghl->addWidget(b);
        m_galleryModeBtn[mode] = b;
    };
    layoutButton(Icon::LayoutReel, tr("Reel"), tr("Captured images in a row under the live image"), GalleryReel);
    layoutButton(Icon::LayoutList, tr("List"), tr("Captured images in a list beside the live image, with their names"),
                 GalleryList);
    layoutButton(Icon::LayoutCompact, tr("Compact"),
                 tr("Captured images as a compact list: small thumbnails and names, many at once"), GalleryCompact);
    gbl->addWidget(gh);
    m_gallery = new GalleryWidget(galleryBox);
    gbl->addWidget(m_gallery, 1);
    centre->addWidget(m_view);
    centre->addWidget(galleryBox);
    centre->setStretchFactor(0, 5);
    centre->setStretchFactor(1, 1);
    split->addWidget(centre);
    applyGalleryLayout(AppSettings::instance().galleryVertical);
    // the split above was made before the window had its size (the list then
    // took half the image's room); once it is shown, make it again
    QTimer::singleShot(0, this, [this] { applyGalleryLayout(AppSettings::instance().galleryVertical); });

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
    m_rightPanel = panelScroll(right, panelWidth());
    split->addWidget(m_rightPanel);

    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    split->setSizes({panelWidth() + px(20), px(1100), panelWidth() + px(20)});
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
    updateCaptureState();
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
    file->addAction(icon(Icon::Save), tr("Export captured images to a Leica &.lif…"), this,
                    &MainWindow::exportSessionToLif);
    file->addSeparator();
    file->addAction(icon(Icon::Folder), tr("Open image &folder"), this, [] {
        const QString d = AppSettings::instance().capture.folder;
        QDir().mkpath(d);
        QDesktopServices::openUrl(QUrl::fromLocalFile(d));
    });
    file->addSeparator();
    auto *settingsAction = file->addAction(icon(Icon::Settings), tr("&Settings…"), QKeySequence(tr("Ctrl+,")), this, [this] {
        SettingsDialog dlg(this);
        connect(&dlg, &SettingsDialog::appearanceChanged, this,
                [this](const QString &t, int scale) { applyAppearance(t, scale); });
        bool reset = false;
        connect(&dlg, &SettingsDialog::resetAllRequested, this, [&reset] { reset = true; });
        if (dlg.exec() == QDialog::Accepted) {
            setGalleryMode(dlg.galleryMode());
            m_capturePanel->refreshFromSettings();
            updateNextName();
        }
        if (reset) {
            // quit the normal way, so an unsaved result or a running capture is
            // asked about first; closeEvent() erases the settings once it is
            // sure the application is closing
            m_resetSettingsOnClose = true;
            if (!close())
                m_resetSettingsOnClose = false;
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
    // each workspace has its own image; the zoom commands act on the one shown
    auto zoomAction = [this](void (ImageView::*zoom)()) {
        return [this, zoom] {
            if (ImageView *v = currentImageView())
                (v->*zoom)();
        };
    };
    view->addAction(icon(Icon::ZoomFit), tr("Zoom to &fit"), QKeySequence(tr("Ctrl+0")), this, zoomAction(&ImageView::zoomFit));
    view->addAction(icon(Icon::ZoomActual), tr("Actual &pixels"), QKeySequence(tr("Ctrl+Alt+0")), this,
                    zoomAction(&ImageView::zoomActual));
    view->addAction(icon(Icon::ZoomIn), tr("Zoom &in"), QKeySequence::ZoomIn, this, zoomAction(&ImageView::zoomIn));
    view->addAction(icon(Icon::ZoomOut), tr("Zoom &out"), QKeySequence::ZoomOut, this, zoomAction(&ImageView::zoomOut));
    view->addSeparator();
    // reference overlay: an earlier capture over the live image, to find the same
    // area on the next serial section
    QMenu *refMenu = view->addMenu(tr("&Reference overlay"));
    refMenu->setIcon(icon(Icon::Compare));
    m_referenceAct = refMenu->addAction(tr("Show &selected image over the live image"));
    m_referenceAct->setCheckable(true);
    m_referenceAct->setShortcut(referenceOverlayShortcut());
    connect(m_referenceAct, &QAction::triggered, this, [this](bool on) {
        if (!on) {
            showReference(QString());
            return;
        }
        QListWidgetItem *it = m_gallery->currentItem() ? m_gallery->currentItem() : m_gallery->item(0);
        if (!it) {
            m_referenceAct->setChecked(false);
            showMessage(tr("Capture an image first (or select one in the strip below the live image)"), 6000);
            return;
        }
        showReference(it->data(Qt::UserRole).toString());
    });
    auto *opacityGroup = new QActionGroup(this);
    static const QString opacityKey = QStringLiteral("ui/referenceOpacity");
    const int savedOpacity = QSettings().value(opacityKey, 50).toInt();
    for (int pct : {25, 50, 75}) {
        QAction *a = refMenu->addAction(tr("%1% opacity").arg(pct));
        a->setCheckable(true);
        a->setChecked(pct == savedOpacity);
        opacityGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, pct] {
            QSettings().setValue(opacityKey, pct);
            m_view->setReferenceOpacity(pct / 100.0);
        });
    }
    m_view->setReferenceOpacity(savedOpacity / 100.0);
    view->addSeparator();
    {
        QMenu *strip = view->addMenu(icon(Icon::LayoutReel), tr("&Captured images"));
        auto *group = new QActionGroup(this);
        m_galleryModeAct[GalleryReel] = strip->addAction(icon(Icon::LayoutReel), tr("&Reel under the live image"));
        m_galleryModeAct[GalleryList] = strip->addAction(icon(Icon::LayoutList), tr("&List beside the live image"));
        m_galleryModeAct[GalleryCompact] =
            strip->addAction(icon(Icon::LayoutCompact), tr("&Compact list (small thumbnails, many images)"));
        for (int m = 0; m < 3; ++m) {
            m_galleryModeAct[m]->setCheckable(true);
            group->addAction(m_galleryModeAct[m]);
            connect(m_galleryModeAct[m], &QAction::triggered, this, [this, m] { setGalleryMode(m); });
        }
        const auto &S = AppSettings::instance();
        syncGalleryControls(!S.galleryVertical ? GalleryReel : S.galleryCompact ? GalleryCompact : GalleryList);
    }
    view->addSeparator();
    QMenu *size = view->addMenu(tr("&Interface size"));
    size->setIcon(icon(Icon::Settings));
    size->addAction(icon(Icon::Plus), tr("&Larger text"), QKeySequence(QStringLiteral("Ctrl+Shift+=")), this, [this] {
        const auto &S = AppSettings::instance();
        applyAppearance(S.theme, nextUiScale(S.uiScale, +1), true);
    });
    size->addAction(icon(Icon::Minus), tr("&Smaller text"), QKeySequence(QStringLiteral("Ctrl+Shift+-")), this, [this] {
        const auto &S = AppSettings::instance();
        applyAppearance(S.theme, nextUiScale(S.uiScale, -1), true);
    });
    size->addAction(icon(Icon::Reset), tr("&Default size"), QKeySequence(QStringLiteral("Ctrl+Shift+0")), this, [this] {
        applyAppearance(AppSettings::instance().theme, 100, true);
    });
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
            // returns at once; the installer reports when it is done
            setUpCameraAccess(this, [this](const QString &r) {
                if (!r.isEmpty())
                    showMessage(r, 6000);
                m_cameraPanel->refreshCameras();
            });
        });
    tools->addAction(icon(Icon::Refresh), tr("&Search cameras"), m_cameraPanel, &CameraPanel::refreshCameras);
    tools->addSeparator();
    tools->addAction(icon(Icon::Calibrate), tr("Correct &pixel size of saved images…"), this, [this] {
        CalibrationRepairDialog dlg(AppSettings::instance().capture.folder, MicroscopeConfig::kAssumedAdapterBefore,
                                    m_scope.adapterFactor, this);
        dlg.exec();
    });

    // ---- Help
    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&User guide"), QKeySequence(Qt::Key_F1), this, [this] {
        // installed: <app>/docs/USER_GUIDE.html (Windows), <bundle>/Contents/Resources (macOS),
        // <prefix>/share/doc/DMImaging (Linux); development tree: <repo>/docs/USER_GUIDE.md
        QDir d(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 5; ++i) {
            for (const char *name : {"docs/USER_GUIDE.html", "docs/USER_GUIDE.md", "Resources/USER_GUIDE.md",
                                     "share/doc/DMImaging/USER_GUIDE.md"}) {
                const QString p = d.filePath(QString::fromLatin1(name));
                if (QFileInfo::exists(p)) {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(p));
                    return;
                }
            }
            if (!d.cdUp())
                break;
        }
        QMessageBox::information(this, tr("User guide"), tr("The user guide was not found. Reinstall DM Imaging."));
    });
    help->addSeparator();
    help->addAction(icon(Icon::Help), tr("&Keyboard shortcuts"), this, [this] {
        // Every combination is the one the menus really use, spelled the way
        // this platform does (Qt maps Ctrl to Command on macOS, and Redo is
        // Ctrl+Y on Windows but Shift+Ctrl+Z elsewhere).
        auto key = [](const QKeySequence &k) { return k.toString(QKeySequence::NativeText).toHtmlEscaped(); };
        // the Ctrl (Command) modifier on its own, for "Ctrl+drag"
        QString ctrl = QKeySequence(Qt::CTRL | Qt::Key_A).toString(QKeySequence::NativeText);
        ctrl.chop(1);
        QMessageBox box(QMessageBox::NoIcon, tr("Keyboard shortcuts"),
                        tr("<b>Camera</b><br>"
                           "F5 — live on/off · F6 — freeze<br>"
                           "F7 — auto white balance · F8 — auto exposure once<br>"
                           "F9 or Space — capture image · F1 — user guide<br>"
                           "%1 — reference overlay (earlier image over the live image)<br>"
                           "%2, %3, … — select objective<br><br>"
                           "<b>Workspaces</b><br>"
                           "%4 / %5 / %6 — Acquire / Browse / Process · F11 — full screen<br><br>"
                           "<b>Image</b><br>"
                           "Mouse wheel over the image — zoom<br>"
                           "Double click — fit / 100%  ·  0 / 1 / 2 — fit / 100% / 200%<br>"
                           "%7drag or middle drag — pan<br>"
                           "%8 — fit · %9 / %10 — zoom in / out<br><br>"
                           "<b>Annotations</b><br>"
                           "Del — delete selected · %11 / %12 — undo / redo<br><br>"
                           "<b>Side panels</b><br>"
                           "The mouse wheel scrolls the panel. It never changes a setting: "
                           "drag a slider, type in the box, or use the arrow keys.<br><br>"
                           "<b>Interface size</b><br>"
                           "%13 / %14 — larger / smaller text<br>"
                           "%15 — back to the default size")
                            .arg(key(referenceOverlayShortcut()), key(QKeySequence(QStringLiteral("Ctrl+1"))),
                                 key(QKeySequence(QStringLiteral("Ctrl+2"))), key(QKeySequence(tr("Alt+1"))),
                                 key(QKeySequence(tr("Alt+2"))), key(QKeySequence(tr("Alt+3"))), ctrl.toHtmlEscaped(),
                                 key(QKeySequence(tr("Ctrl+0"))), key(QKeySequence(QKeySequence::ZoomIn)))
                            .arg(key(QKeySequence(QKeySequence::ZoomOut)), key(QKeySequence(QKeySequence::Undo)),
                                 key(QKeySequence(QKeySequence::Redo)),
                                 key(QKeySequence(QStringLiteral("Ctrl+Shift+="))),
                                 key(QKeySequence(QStringLiteral("Ctrl+Shift+-"))),
                                 key(QKeySequence(QStringLiteral("Ctrl+Shift+0")))),
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

void MainWindow::openPaths(const QStringList &paths)
{
    QStringList files;
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        if (!fi.exists()) {
            showMessage(tr("%1 does not exist").arg(QDir::toNativeSeparators(p)), 8000);
            continue;
        }
        if (fi.isDir()) {
            m_browse->setFolder(fi.absoluteFilePath());
            m_tabs->setCurrentIndex(1);
            continue;
        }
        files << fi.absoluteFilePath();
    }
    if (files.isEmpty())
        return;
    // Process shows one image; the others are a click away in Browse
    m_browse->setFolder(QFileInfo(files.first()).absolutePath());
    if (m_process->openFile(files.first()))
        m_tabs->setCurrentIndex(2);
    if (files.size() > 1)
        showMessage(tr("Opened %1; the other %n image(s) are in Browse", nullptr, int(files.size() - 1))
                        .arg(QFileInfo(files.first()).fileName()),
                    8000);
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

void MainWindow::exportSessionToLif()
{
    const QStringList paths = m_gallery->paths();
    if (paths.isEmpty()) {
        QMessageBox::information(this, tr("Export to .lif"),
                                 tr("No images have been captured in this session yet.\n\n"
                                    "A .lif holds a whole session, the way LAS X saves one; capture some images "
                                    "first, or open the files you want in Process and save them individually."));
        return;
    }
    const auto &S = AppSettings::instance();
    QString sample = S.capture.sample.trimmed();
    if (sample.isEmpty())
        sample = QStringLiteral("session");
    const QString suggested =
        QDir(S.capture.folder)
            .filePath(QStringLiteral("%1_%2.lif").arg(sample, QDateTime::currentDateTime().toString(
                                                                  QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString path = lm::getSaveFileName(this, tr("Export captured images to a Leica .lif"), suggested,
                                             tr("Leica image file (*.lif)"));
    if (path.isEmpty())
        return;

    // Read each captured file back and collect it. The images are 8-bit in the
    // file LAS X expects, which is also what it writes itself.
    QList<LifImageOut> images;
    QStringList failed;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const QString &p : paths) {
        LoadedImage li;
        QString err;
        if (!loadImage(p, li, &err)) {
            failed << QFileInfo(p).fileName();
            continue;
        }
        LifImageOut out;
        out.name = QFileInfo(p).completeBaseName();
        out.data = std::move(li.data);
        out.umPerPixel = li.meta.umPerPixel;
        out.eightBit = true;
        images.push_back(std::move(out));
    }
    QString err;
    const bool ok = !images.isEmpty() && writeLif(path, images, sample, &err);
    QApplication::restoreOverrideCursor();

    if (!ok) {
        QMessageBox::warning(this, tr("Export to .lif"),
                             images.isEmpty() ? tr("None of the captured images could be read back.")
                                              : tr("Writing %1 failed:\n%2").arg(path, err));
        return;
    }
    QString msg = tr("Wrote %n image(s) to %1", nullptr, int(images.size())).arg(QFileInfo(path).fileName());
    if (!failed.isEmpty())
        msg += tr(". These could not be read back and were left out: %1").arg(failed.join(QStringLiteral(", ")));
    showMessage(msg, 10000);
}

void MainWindow::applyGalleryLayout(bool vertical)
{
    if (!m_centreSplitter || !m_gallery)
        return;
    m_gallery->setCompact(AppSettings::instance().galleryCompact);
    m_gallery->setVertical(vertical);
    m_centreSplitter->setOrientation(vertical ? Qt::Horizontal : Qt::Vertical);
    // The splitter keeps the sizes it had, which are meaningless once the
    // orientation flips, so give it a sensible split of the space it has.
    const int total = vertical ? m_centreSplitter->width() : m_centreSplitter->height();
    const int strip = !vertical ? px(150) : AppSettings::instance().galleryCompact ? px(200) : px(250);
    const int image = std::max(px(200), (total > 0 ? total : px(950)) - strip);
    m_centreSplitter->setSizes({image, strip});
}

void MainWindow::syncGalleryControls(int mode)
{
    for (int m = 0; m < 3; ++m) {
        if (m_galleryModeAct[m])
            m_galleryModeAct[m]->setChecked(m == mode);
        if (m_galleryModeBtn[m] && m == mode) {
            // the group unchecks the others; no signal back into setGalleryMode()
            const QSignalBlocker block(m_galleryModeBtn[m]);
            m_galleryModeBtn[m]->setChecked(true);
        }
    }
}

void MainWindow::setGalleryMode(int mode)
{
    syncGalleryControls(mode);
    auto &S = AppSettings::instance();
    const bool vertical = mode != GalleryReel, compact = mode == GalleryCompact;
    if (vertical == S.galleryVertical && compact == S.galleryCompact && m_gallery
        && m_gallery->isVertical() == vertical && m_gallery->isCompact() == compact)
        return;
    S.galleryVertical = vertical;
    S.galleryCompact = compact;
    S.save();
    applyGalleryLayout(vertical);
    showMessage(compact    ? tr("Captured images: compact list beside the image")
                : vertical ? tr("Captured images: list beside the image")
                           : tr("Captured images: reel under the image"),
                2500);
}

void MainWindow::setCameraLed(CameraState state)
{
    m_cameraState = state;
    const QColor c = state == CameraState::Live      ? theme().success
                     : state == CameraState::Ready   ? theme().accent
                     : state == CameraState::Lost    ? theme().danger
                                                     : theme().faintText;
    const int d = px(9);
    QPixmap pm(QSize(d, d) * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(0, 0, d, d));
    p.end();
    m_statusLed->setPixmap(pm);
}

void MainWindow::applyAppearance(const QString &theme, int scalePercent, bool save)
{
    auto &S = AppSettings::instance();
    const bool scaleChanged = scalePercent != uiScale();
    S.theme = theme;
    S.uiScale = scalePercent;
    applyTheme(*qApp, theme, scalePercent);
    // the style sheet and font reach every widget on their own; icon sizes do not
    applyUiScaleTo(this);
    setCameraLed(m_cameraState); // redrawn at the new size and in the new colours
    if (scaleChanged)
        applyGalleryLayout(AppSettings::instance().galleryVertical);
    if (scaleChanged) {
        // the panels have a minimum width in device pixels, and the splitter
        // keeps whatever widths it had: both have to be told about the change
        const int w = panelWidth();
        m_leftPanel->setMinimumWidth(w);
        m_rightPanel->setMinimumWidth(w);
        if (m_acquireSplitter) {
            QList<int> sizes = m_acquireSplitter->sizes();
            if (sizes.size() == 3) {
                const int centre = std::max(px(200), sizes[0] + sizes[1] + sizes[2] - 2 * w);
                m_acquireSplitter->setSizes({w, centre, w});
            }
        }
    }
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w != this)
            applyUiScaleTo(w);
    if (scaleChanged)
        showMessage(tr("Interface size %1%").arg(scalePercent), 2500);
    if (save)
        S.save();
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
    // A camera can report a resolution it then does not deliver: a MacBook
    // camera hands out 1920x1080 from a square sensor format. The backend
    // corrects its list from the first frame of a new size, so the panel that
    // shows it has to be refreshed once.
    if (stats.width > 0 && stats.height > 0 && QSize(stats.width, stats.height) != m_liveFrameSize) {
        m_liveFrameSize = QSize(stats.width, stats.height);
        m_cameraPanel->syncFromCamera();
    }
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
    updateCaptureState();
    Camera *cam = m_engine->camera();
    if (!cam) {
        m_statusCamera->setText(tr("No camera"));
        setCameraLed(m_lostCameraId.isEmpty() ? CameraState::None : CameraState::Lost);
        m_capturePanel->setShotModes({});
        m_view->clear();
        updateTitle();
        return;
    }
    m_statusCamera->setText(QString::fromStdString(cam->info().name));
    setCameraLed(m_engine->isLive() ? CameraState::Live : CameraState::Ready);
    QStringList modes;
    for (const auto &m : cam->shotModes())
        modes << QString::fromStdString(m.name);
    m_capturePanel->setShotModes(modes, cam->supportsHdr());
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
        // With auto exposure on too: what this objective last settled at is the
        // best place to start, and auto exposure only fine-tunes from there
        // (starting from the previous objective's exposure made it search).
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
        m_toolsPanel->resetFocusPeak(); // another magnification: another sharpness scale
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
        // Kept in sensor pixels, converted at the moment of the click: the live
        // preview switches between half and full resolution with the zoom, so
        // the display scale at the second click need not be the one at the first.
        const double s = m_lastStats.displayScale > 0 ? m_lastStats.displayScale : 1.0;
        m_calibPoints.append(QPointF(p) / s);
        if (m_calibPoints.size() == 1) {
            m_view->startPick(ImageView::PickMode::Point, tr("Calibration: click the SECOND mark"));
            return;
        }
        disconnect(m_view, &ImageView::pointPicked, nullptr, nullptr);
        const double dx = m_calibPoints[1].x() - m_calibPoints[0].x();
        const double dy = m_calibPoints[1].y() - m_calibPoints[0].y();
        const double px = std::hypot(dx, dy);
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
    updateCaptureState();
    if (c.shotMode >= 0 && !m_engine->camera()->shotModes().empty()) {
        m_capturePanel->setBusy(true, tr("Pixel shift capture…"));
        m_engine->captureShots(c.shotMode);
    } else if (isHdrMode(c.shotMode) && m_engine->camera()->supportsHdr()) {
        m_capturePanel->setBusy(true, tr("HDR capture…"));
        m_engine->captureHdr(hdrExposures(c.shotMode), c.averageFrames);
    } else {
        m_capturePanel->setBusy(true);
        m_engine->capture(c.averageFrames);
        // watchdog: if the camera stops delivering frames the capture would wait forever
        // (pixel-shift captures have their own per-shot timeouts)
        const int serial = ++m_captureSerial;
        const double frameMs = std::max(40.0, m_engine->camera()->exposure());
        const int timeoutMs = int(std::max(1, c.averageFrames) * frameMs) + 5000;
        QTimer::singleShot(timeoutMs, this, [this, serial] {
            if (m_capturing && serial == m_captureSerial && !m_engine->isBusy())
                m_engine->cancelPendingCapture(tr("No image from the camera. Check that the live image is running "
                                                  "(F5) and try again."));
        });
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
        // the serial the camera itself reports (the DMC6200 reads it over its
        // protocol when opened), not one derived from the USB device path
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
    // the same choice MicroscopeConfig::umPerPixel() makes
    m.pixelSizeSource = QString::fromLatin1(o.calibratedUmPerPixel > 0 ? kPixelSizeCalibrated : kPixelSizeNominal);
    m.exposureMs = r.exposureMs;
    m.exposureSeriesMs = QVector<double>(r.exposureSeriesMs.begin(), r.exposureSeriesMs.end());
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
    updateCaptureState();
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
                          [&](int obj) { return QFileInfo(suggested(obj)).completeBaseName(); }, info,
                          S.capture.folder, this);
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
        if (QDir::cleanPath(dlg.folder()) != QDir::cleanPath(S.capture.folder)) {
            // a folder chosen here is where the following images go too
            S.capture.folder = QDir::cleanPath(dlg.folder());
            QDir().mkpath(S.capture.folder);
            m_capturePanel->refreshFromSettings();
            showMessage(tr("Images are now saved in %1").arg(QDir::toNativeSeparators(S.capture.folder)), 6000);
        }
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
    updateCaptureState();
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
        updateCaptureState();
        if (!err.isEmpty()) {
            QMessageBox::warning(this, tr("Save image"), tr("Could not save %1:\n%2").arg(path, err));
            return;
        }
        m_gallery->addImage(path, thumbSrc);
        showMessage(tr("Saved %1 (%2 × %3)").arg(QDir::toNativeSeparators(path)).arg(r->rendered16.width).arg(r->rendered16.height), 6000);
        if (AppSettings::instance().capture.openInProcess && !m_timelapse.isActive()) {
            if (m_process->hasUnsavedResult()) { // don't replace an unsaved result without asking
                showMessage(tr("Saved %1 (not opened in Process: the image there has not been saved)")
                                .arg(QFileInfo(path).fileName()),
                            8000);
            } else {
                m_process->openImage(r->rendered16, meta, path);
                m_tabs->setCurrentIndex(2);
            }
        }
    });
}

void MainWindow::updateCaptureState()
{
    QString busy;
    if (m_capturing)
        busy = tr("Capturing…");
    else if (m_pendingSaves.size() > 1)
        busy = tr("Saving %n images…", nullptr, int(m_pendingSaves.size()));
    else if (!m_pendingSaves.isEmpty())
        busy = tr("Saving…");
    m_view->setBusy(busy);
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
    updateCaptureState();
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

void MainWindow::showReference(const QString &path)
{
    m_referencePath = path;
    if (path.isEmpty()) {
        m_view->setReferenceImage(QImage());
        m_referenceAct->setChecked(false);
        return;
    }
    m_referenceAct->setChecked(true);
    QtConcurrent::run([path] {
        LoadedImage li;
        if (!loadImage(path, li))
            return QImage();
        // a screen never needs more than ~2560 px; reduce before converting, and
        // store as RGB32, which QPainter blends with opacity on its fast path
        const int factor = (std::max(li.data.width, li.data.height) + 2559) / 2560;
        if (factor > 1)
            li.data = downscale(li.data, factor);
        return toQImage8(li.data).convertToFormat(QImage::Format_RGB32);
    }).then(this, [this, path](const QImage &img) {
        if (path != m_referencePath) // replaced or hidden while loading
            return;
        if (img.isNull()) {
            showReference(QString());
            showMessage(tr("Cannot read %1").arg(QDir::toNativeSeparators(path)), 8000);
            return;
        }
        m_tabs->setCurrentIndex(0);
        m_view->setReferenceImage(img);
        if (!m_view->image().isNull() && !m_view->referenceFits()) {
            showMessage(tr("Reference overlay: %1 has a different shape from the live image (image format or "
                           "live stitching?); it is shown once they match.")
                            .arg(QFileInfo(path).fileName()),
                        0);
            return;
        }
        showMessage(tr("Reference overlay: %1. Move the stage until the images match; %2 hides it. "
                       "Opacity: View → Reference overlay.")
                        .arg(QFileInfo(path).fileName(),
                             m_referenceAct->shortcut().toString(QKeySequence::NativeText)),
                    0);
    });
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

ImageView *MainWindow::currentImageView() const
{
    switch (m_stack->currentIndex()) {
    case 0: return m_view;
    case 1: return m_browse->imageView();
    case 2: return m_process->imageView();
    default: return nullptr;
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!m_process->maybeDiscardUnsaved()) {
        e->ignore();
        return;
    }
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
    m_process->flushAnnotations();
    if (m_resetSettingsOnClose) {
        // Settings → Reset all settings: nothing of this session is written
        // back, or the reset would be undone on the way out
        m_engine->closeCamera();
        QSettings qs;
        qs.clear();
        qs.sync();
        e->accept();
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }
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
