#include "CameraPanel.h"

#include "app/AcquisitionEngine.h"
#include "app/AppSettings.h"
#include "ui/CollapsibleSection.h"
#include "ui/SliderSpin.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace lm {

CameraPanel::CameraPanel(AcquisitionEngine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    auto &S = AppSettings::instance();
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- device
    auto *dev = new CollapsibleSection(tr("Camera"), this);
    auto *row = new QHBoxLayout;
    m_cameraCombo = new QComboBox(this);
    m_cameraCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_cameraCombo->setMinimumContentsLength(18);
    auto *refresh = new QToolButton(this);
    refresh->setText(QStringLiteral("⟳"));
    refresh->setToolTip(tr("Search for cameras"));
    row->addWidget(m_cameraCombo, 1);
    row->addWidget(refresh);
    dev->contentLayout()->addLayout(row);
    m_connect = new QPushButton(tr("Connect"), this);
    dev->contentLayout()->addWidget(m_connect);
    m_info = new QLabel(this);
    m_info->setObjectName(QStringLiteral("Hint"));
    m_info->setWordWrap(true);
    dev->contentLayout()->addWidget(m_info);
    auto *liveRow = new QHBoxLayout;
    m_live = new QPushButton(tr("Live"), this);
    m_live->setObjectName(QStringLiteral("LiveButton"));
    m_live->setCheckable(true);
    m_live->setToolTip(tr("Start/stop live image (F5)"));
    m_freeze = new QPushButton(tr("Freeze"), this);
    m_freeze->setCheckable(true);
    m_freeze->setToolTip(tr("Freeze the displayed image while the camera keeps running (F6)"));
    liveRow->addWidget(m_live);
    liveRow->addWidget(m_freeze);
    dev->contentLayout()->addLayout(liveRow);
    root->addWidget(dev);

    // --- acquisition
    auto *acq = new CollapsibleSection(tr("Exposure"), this);
    m_resolution = new QComboBox(this);
    auto *resLabel = new QLabel(tr("Image format"), this);
    resLabel->setObjectName(QStringLiteral("ControlLabel"));
    acq->contentLayout()->addWidget(resLabel);
    acq->contentLayout()->addWidget(m_resolution);
    m_exposure = new SliderSpin(tr("Exposure time"), 0.026, 5000.0, 3, this, true, tr(" ms"));
    m_exposure->spinBox()->setMaximum(60000);
    m_exposure->setValue(S.exposureMs);
    acq->contentLayout()->addWidget(m_exposure);
    m_autoExposure = new QCheckBox(tr("Auto exposure"), this);
    m_autoExposure->setChecked(S.autoExposure);
    m_aeOnce = new QPushButton(tr("Auto once"), this);
    m_aeOnce->setToolTip(tr("Adjust the exposure once, then keep it fixed"));
    auto *aeRow = new QHBoxLayout;
    aeRow->addWidget(m_autoExposure, 1);
    aeRow->addWidget(m_aeOnce);
    acq->contentLayout()->addLayout(aeRow);
    m_aeTarget = new SliderSpin(tr("Auto exposure brightness"), 30, 98, 0, this, false, tr(" %"));
    m_aeTarget->setValue(S.aeTarget * 100);
    m_aeTarget->setDefault(85);
    m_aeTarget->setToolTip(tr("Brightness of the brightest 1% of the image (bright field background)"));
    acq->contentLayout()->addWidget(m_aeTarget);
    m_gain = new SliderSpin(tr("Gain"), 1.0, 16.0, 2, this, true, tr(" ×"));
    m_gain->setValue(S.gain);
    m_gain->setDefault(1.0);
    acq->contentLayout()->addWidget(m_gain);
    m_aeGain = new QCheckBox(tr("Auto exposure may raise gain"), this);
    acq->contentLayout()->addWidget(m_aeGain);
    m_hqPreview = new QCheckBox(tr("High quality live demosaicing"), this);
    m_hqPreview->setToolTip(tr("Uses the capture-quality demosaicing for the live image (more CPU)"));
    acq->contentLayout()->addWidget(m_hqPreview);
    root->addWidget(acq);

    // --- orientation
    auto *orient = new CollapsibleSection(tr("Orientation"), this, false);
    auto *orow = new QHBoxLayout;
    auto *flipH = new QPushButton(tr("Flip ↔"), this);
    auto *flipV = new QPushButton(tr("Flip ↕"), this);
    auto *rot = new QPushButton(tr("Rotate 90°"), this);
    flipH->setCheckable(true);
    flipV->setCheckable(true);
    flipH->setChecked(S.color.flipHorizontal);
    flipV->setChecked(S.color.flipVertical);
    orow->addWidget(flipH);
    orow->addWidget(flipV);
    orow->addWidget(rot);
    orient->contentLayout()->addLayout(orow);
    root->addWidget(orient);

    m_advanced = new CollapsibleSection(tr("Camera details"), this, false);
    root->addWidget(m_advanced);

    connect(refresh, &QToolButton::clicked, this, &CameraPanel::refreshCameras);
    connect(m_connect, &QPushButton::clicked, this, &CameraPanel::onConnectClicked);
    connect(m_live, &QPushButton::toggled, this, &CameraPanel::onLiveToggled);
    connect(m_freeze, &QPushButton::toggled, this, [this](bool on) { m_engine->setFrozen(on); });
    connect(m_resolution, &QComboBox::activated, this, [this](int i) {
        if (m_engine->camera() && m_engine->camera()->setResolutionIndex(i)) {
            AppSettings::instance().resolutionIndex = i;
            emit message(tr("Image format: %1").arg(m_resolution->currentText()), 3000);
        }
    });
    connect(m_exposure, &SliderSpin::valueChanged, this, [this](double v) {
        if (m_engine->camera())
            m_engine->camera()->setExposure(v);
        AppSettings::instance().exposureMs = v;
    });
    auto updateAe = [this] {
        AutoExposureSettings ae = m_engine->autoExposure();
        ae.enabled = m_autoExposure->isChecked();
        ae.target = m_aeTarget->value() / 100.0;
        ae.allowGain = m_aeGain->isChecked();
        m_engine->setAutoExposure(ae);
        auto &S2 = AppSettings::instance();
        S2.autoExposure = ae.enabled;
        S2.aeTarget = ae.target;
        m_exposure->setEnabledControls(!ae.enabled);
        m_gain->setEnabledControls(!(ae.enabled && ae.allowGain));
    };
    connect(m_autoExposure, &QCheckBox::toggled, this, updateAe);
    connect(m_aeGain, &QCheckBox::toggled, this, updateAe);
    connect(m_aeTarget, &SliderSpin::valueChanged, this, updateAe);
    connect(m_aeOnce, &QPushButton::clicked, this, [this, updateAe] {
        updateAe();
        m_engine->requestAutoExposureOnce();
    });
    connect(m_gain, &SliderSpin::valueChanged, this, [this](double v) {
        if (m_engine->camera())
            m_engine->camera()->setGain(v);
        AppSettings::instance().gain = v;
    });
    connect(m_hqPreview, &QCheckBox::toggled, this, [this](bool on) { m_engine->setPreviewQuality(on); });
    connect(flipH, &QPushButton::toggled, this, [this](bool on) {
        auto c = m_engine->colorSettings();
        c.flipHorizontal = on;
        m_engine->setColorSettings(c);
        AppSettings::instance().color = c;
        emit orientationChanged();
    });
    connect(flipV, &QPushButton::toggled, this, [this](bool on) {
        auto c = m_engine->colorSettings();
        c.flipVertical = on;
        m_engine->setColorSettings(c);
        AppSettings::instance().color = c;
        emit orientationChanged();
    });
    connect(rot, &QPushButton::clicked, this, [this] {
        auto c = m_engine->colorSettings();
        c.rotation = (c.rotation + 90) % 360;
        m_engine->setColorSettings(c);
        AppSettings::instance().color = c;
        emit orientationChanged();
        emit message(tr("Rotation %1°").arg(c.rotation), 2000);
    });
    connect(m_engine, &AcquisitionEngine::exposureChanged, this, &CameraPanel::setExposureDisplay);
    connect(m_engine, &AcquisitionEngine::liveStateChanged, this, [this](bool live) {
        QSignalBlocker b(m_live);
        m_live->setChecked(live);
        m_live->setText(live ? tr("Stop") : tr("Live"));
        updateEnabled();
    });
    updateAe();
    updateEnabled();
}

void CameraPanel::refreshCameras()
{
    const QString current = m_cameraCombo->currentData().toString();
    m_cameras = m_engine->enumerateCameras();
    m_cameraCombo->clear();
    for (const auto &c : m_cameras)
        m_cameraCombo->addItem(QString::fromStdString(c.name), QString::fromStdString(c.id));
    int idx = m_cameraCombo->findData(current);
    if (idx < 0)
        idx = m_cameraCombo->findData(AppSettings::instance().lastCameraId);
    if (idx >= 0)
        m_cameraCombo->setCurrentIndex(idx);
    updateEnabled();
}

void CameraPanel::autoConnect()
{
    refreshCameras();
    // prefer the last used camera, otherwise the first real (non simulator) camera
    int idx = m_cameraCombo->findData(AppSettings::instance().lastCameraId);
    if (idx < 0)
        for (int i = 0; i < int(m_cameras.size()); ++i)
            if (m_cameras[size_t(i)].backend == "Leica USB") {
                idx = i;
                break;
            }
    if (idx >= 0 && m_cameras[size_t(idx)].backend != "Simulator") {
        m_cameraCombo->setCurrentIndex(idx);
        if (connectCamera(idx)) {
            QString err;
            m_engine->startLive(err);
        }
    }
}

bool CameraPanel::connectCamera(int index)
{
    if (index < 0 || index >= int(m_cameras.size()))
        return false;
    QString err;
    emit message(tr("Connecting to %1…").arg(QString::fromStdString(m_cameras[size_t(index)].name)), 0);
    if (!m_engine->openCamera(m_cameras[size_t(index)], err)) {
        emit message(tr("Cannot open camera: %1").arg(err), 10000);
        m_info->setText(tr("Error: %1").arg(err));
        updateEnabled();
        return false;
    }
    auto &S = AppSettings::instance();
    S.lastCameraId = QString::fromStdString(m_cameras[size_t(index)].id);
    Camera *cam = m_engine->camera();
    const auto res = cam->resolutions();
    if (S.resolutionIndex >= 0 && S.resolutionIndex < int(res.size()))
        cam->setResolutionIndex(S.resolutionIndex);
    cam->setExposure(S.exposureMs);
    cam->setGain(S.gain);
    syncFromCamera();
    emit message(tr("Connected: %1").arg(QString::fromStdString(m_cameras[size_t(index)].name)), 4000);
    emit cameraChanged();
    return true;
}

void CameraPanel::syncFromCamera()
{
    Camera *cam = m_engine->camera();
    m_resolution->clear();
    if (!cam) {
        m_info->clear();
        updateEnabled();
        return;
    }
    for (const auto &r : cam->resolutions())
        m_resolution->addItem(QString::fromStdString(r.label));
    m_resolution->setCurrentIndex(cam->resolutionIndex());
    const Range er = cam->exposureRange();
    m_exposure->setRange(er.min, std::min(er.max, 60000.0));
    m_exposure->setValue(cam->exposure());
    const Range gr = cam->gainRange();
    m_gain->setRange(gr.min, std::max(gr.min + 0.01, gr.max));
    m_gain->setValue(cam->gain());
    QStringList lines;
    for (const auto &[k, v] : cam->details())
        lines << QStringLiteral("%1: %2").arg(QString::fromStdString(k), QString::fromStdString(v));
    if (lines.isEmpty())
        lines << QString::fromStdString(cam->info().backend);
    m_info->setText(lines.join(QLatin1Char('\n')));
    buildAdvanced();
    updateEnabled();
}

void CameraPanel::buildAdvanced()
{
    QLayoutItem *it;
    while ((it = m_advanced->contentLayout()->takeAt(0))) {
        delete it->widget();
        delete it;
    }
    Camera *cam = m_engine->camera();
    if (!cam)
        return;
    auto *w = new QWidget(m_advanced);
    m_advancedForm = new QFormLayout(w);
    m_advancedForm->setContentsMargins(0, 0, 0, 0);
    for (const auto &p : cam->properties()) {
        auto *spin = new QDoubleSpinBox(w);
        spin->setRange(p.min, p.max);
        spin->setSingleStep(p.step);
        spin->setDecimals(p.step < 1 ? 2 : 0);
        spin->setValue(p.value);
        spin->setReadOnly(p.readOnly);
        spin->setKeyboardTracking(false);
        const std::string key = p.key;
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key](double v) {
            if (m_engine->camera())
                m_engine->camera()->setProperty(key, v);
        });
        m_advancedForm->addRow(QString::fromStdString(p.label), spin);
    }
    if (m_advancedForm->rowCount() == 0)
        m_advancedForm->addRow(new QLabel(tr("No additional settings"), w));
    m_advanced->contentLayout()->addWidget(w);
}

void CameraPanel::setExposureDisplay(double ms, double gain)
{
    m_exposure->setValue(ms);
    m_gain->setValue(gain);
    AppSettings::instance().exposureMs = ms;
    AppSettings::instance().gain = gain;
}

void CameraPanel::onConnectClicked()
{
    if (m_engine->camera()) {
        m_engine->closeCamera();
        syncFromCamera();
        emit cameraChanged();
        emit message(tr("Camera disconnected"), 3000);
        updateEnabled();
        return;
    }
    if (connectCamera(m_cameraCombo->currentIndex())) {
        QString err;
        if (!m_engine->startLive(err))
            emit message(tr("Cannot start live image: %1").arg(err), 8000);
    }
}

void CameraPanel::onLiveToggled(bool on)
{
    if (on) {
        if (!m_engine->camera() && !connectCamera(m_cameraCombo->currentIndex())) {
            QSignalBlocker b(m_live);
            m_live->setChecked(false);
            return;
        }
        QString err;
        if (!m_engine->startLive(err)) {
            emit message(tr("Cannot start live image: %1").arg(err), 8000);
            QSignalBlocker b(m_live);
            m_live->setChecked(false);
        }
    } else {
        m_engine->stopLive();
    }
    updateEnabled();
}

void CameraPanel::updateEnabled()
{
    const bool open = m_engine->camera() != nullptr;
    m_connect->setText(open ? tr("Disconnect") : tr("Connect"));
    m_cameraCombo->setEnabled(!open);
    m_connect->setEnabled(open || m_cameraCombo->count() > 0);
    m_live->setEnabled(open || m_cameraCombo->count() > 0);
    m_freeze->setEnabled(open);
    m_resolution->setEnabled(open);
}

} // namespace lm
