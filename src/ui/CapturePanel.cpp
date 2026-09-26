#include "CapturePanel.h"

#include "app/AppSettings.h"
#include "ui/CollapsibleSection.h"
#include "ui/Icons.h"
#include "ui/PanelGroup.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace lm {

namespace {
struct FormatChoice { const char *label; FileFormat fmt; bool sixteen; };
const FormatChoice kFormats[] = {
    {"TIFF 16-bit (lossless, recommended)", FileFormat::Tiff, true},
    {"TIFF 8-bit (lossless)", FileFormat::Tiff, false},
    {"PNG 16-bit", FileFormat::Png, true},
    {"PNG 8-bit", FileFormat::Png, false},
    {"JPEG", FileFormat::Jpeg, false},
    {"BMP", FileFormat::Bmp, false},
};
} // namespace

CapturePanel::CapturePanel(QWidget *parent) : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Two groups: what every capture does, and the modes that run over several
    // frames. The four advanced modes used to sit as siblings next to "Acquire
    // image", which made the panel read as one long list of equals.
    auto *capGroup = new PanelGroup(tr("Capture"), Icon::Capture, theme().groupCapture, this);
    root->addWidget(capGroup);

    // --- capture
    auto *cap = capGroup->addSection(tr("Acquire image"), Icon::Capture);
    m_capture = new QPushButton(tr("Capture image"), this);
    m_capture->setObjectName(QStringLiteral("PrimaryButton"));
    m_capture->setIcon(icon(Icon::Capture, QColor(Qt::white), 18));
    m_capture->setProperty("lmIconBase", 18);
    m_capture->setIconSize(iconSize(18));
    m_capture->setToolTip(tr("Acquire and save an image (F9 or Space)"));
    m_capture->setMinimumHeight(px(42));
    cap->contentLayout()->addWidget(m_capture);
    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    cap->contentLayout()->addWidget(m_progress);
    m_nextName = new QLabel(this);
    m_nextName->setObjectName(QStringLiteral("Hint"));
    m_nextName->setWordWrap(true);
    cap->contentLayout()->addWidget(m_nextName);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 4, 0, 0);
    m_mode = new QComboBox(this);
    m_mode->setToolTip(tr("HDR merges longer exposures so dark stains are recorded with less noise. "
                          "Pixel shift moves the sensor to record full colour at every pixel (4-shot) or "
                          "to increase the resolution (16/36-shot). The specimen must not move."));
    form->addRow(tr("Mode"), m_mode);
    m_average = new QSpinBox(this);
    m_average->setRange(1, 64);
    m_average->setSuffix(tr(" frames"));
    m_average->setToolTip(tr("Average several frames to reduce noise"));
    form->addRow(tr("Averaging"), m_average);
    cap->contentLayout()->addLayout(form);

    // --- files
    auto *files = capGroup->addSection(tr("Saving"), Icon::Folder, false);
    auto *ff = new QFormLayout;
    ff->setContentsMargins(0, 0, 0, 0);
    auto *folderRow = new QHBoxLayout;
    m_folder = new QLineEdit(this);
    auto *browse = new QToolButton(this);
    browse->setText(QStringLiteral("…"));
    browse->setToolTip(tr("Choose the folder"));
    auto *open = new QToolButton(this);
    open->setIcon(icon(Icon::Folder, theme().subText, 15));
    open->setToolTip(tr("Open the image folder"));
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    folderRow->addWidget(open);
    ff->addRow(tr("Folder"), folderRow);
    m_sample = new QLineEdit(this);
    m_sample->setToolTip(tr("Sample / slide identifier used in file names ({sample})"));
    ff->addRow(tr("Sample"), m_sample);
    m_pattern = new QLineEdit(this);
    m_pattern->setToolTip(tr("Placeholders replaced when saving: {sample} {objective} {date} {time} {counter} {mode} "
                             "{operator}. Example: {sample}_{objective}_{counter} gives Liver01_20x_003."));
    ff->addRow(tr("File name"), m_pattern);
    m_counter = new QSpinBox(this);
    m_counter->setRange(0, 999999);
    m_counter->setToolTip(tr("Number used for {counter} in the next file name"));
    ff->addRow(tr("Next number"), m_counter);
    m_format = new QComboBox(this);
    for (const auto &f : kFormats)
        m_format->addItem(tr(f.label));
    ff->addRow(tr("Format"), m_format);
    m_jpegQuality = new QSpinBox(this);
    m_jpegQuality->setRange(50, 100);
    m_jpegQuality->setSuffix(QStringLiteral(" %"));
    ff->addRow(tr("JPEG quality"), m_jpegQuality);
    files->contentLayout()->addLayout(ff);
    m_burnScale = new QCheckBox(tr("Burn scale bar into saved image"), this);
    m_burnScale->setToolTip(tr("Draws the scale bar into the image pixels permanently. Not recommended for images you "
                               "will analyse (IHC); use Export with overlays in Process instead."));
    files->contentLayout()->addWidget(m_burnScale);
    m_openProcess = new QCheckBox(tr("Open captured image in Process"), this);
    files->contentLayout()->addWidget(m_openProcess);
    m_prompt = new QCheckBox(tr("Ask for magnification and name after capture"), this);
    files->contentLayout()->addWidget(m_prompt);

    // Everything below acquires a series rather than a single image.
    auto *advGroup = new PanelGroup(tr("Advanced acquisition"), Icon::Multifocus, theme().groupCapture, this);
    root->addWidget(advGroup);

    // --- multifocus
    auto *mf = advGroup->addSection(tr("Multifocus (depth of field)"), Icon::Multifocus, false);
    auto *mfHint = new QLabel(tr("Start, then slowly turn the fine focus through the whole thickness of the "
                                 "specimen. Every sharp detail is merged into one image."), this);
    mfHint->setObjectName(QStringLiteral("Hint"));
    mfHint->setWordWrap(true);
    mf->contentLayout()->addWidget(mfHint);
    auto *mfRow = new QHBoxLayout;
    m_mfStart = new QPushButton(tr("Start"), this);
    m_mfStart->setIcon(icon(Icon::Play, theme().text, 15));
    m_mfFinish = new QPushButton(tr("Finish && save"), this);
    m_mfFinish->setIcon(icon(Icon::Check, theme().success, 15));
    m_mfCancel = new QPushButton(tr("Cancel"), this);
    mfRow->addWidget(m_mfStart);
    mfRow->addWidget(m_mfFinish);
    mfRow->addWidget(m_mfCancel);
    mf->contentLayout()->addLayout(mfRow);
    m_mfStatus = new QLabel(this);
    m_mfStatus->setObjectName(QStringLiteral("Hint"));
    mf->contentLayout()->addWidget(m_mfStatus);

    // --- mosaic
    auto *mo = advGroup->addSection(tr("Live image builder (stitching)"), Icon::Stitch, false);
    auto *moHint = new QLabel(tr("Start, then move the stage slowly. New areas are added automatically "
                                 "when the stage rests; overlap with already scanned areas."), this);
    moHint->setObjectName(QStringLiteral("Hint"));
    moHint->setWordWrap(true);
    mo->contentLayout()->addWidget(moHint);
    auto *moRow = new QHBoxLayout;
    m_moStart = new QPushButton(tr("Start"), this);
    m_moStart->setIcon(icon(Icon::Play, theme().text, 15));
    m_moFinish = new QPushButton(tr("Finish && save"), this);
    m_moFinish->setIcon(icon(Icon::Check, theme().success, 15));
    m_moCancel = new QPushButton(tr("Cancel"), this);
    moRow->addWidget(m_moStart);
    moRow->addWidget(m_moFinish);
    moRow->addWidget(m_moCancel);
    mo->contentLayout()->addLayout(moRow);
    auto *moRow2 = new QHBoxLayout;
    m_moAdd = new QPushButton(tr("Add tile now"), this);
    m_moAuto = new QCheckBox(tr("Add automatically"), this);
    m_moAuto->setChecked(true);
    moRow2->addWidget(m_moAdd);
    moRow2->addWidget(m_moAuto);
    mo->contentLayout()->addLayout(moRow2);
    m_moStatus = new QLabel(this);
    m_moStatus->setObjectName(QStringLiteral("Hint"));
    mo->contentLayout()->addWidget(m_moStatus);

    // --- video
    auto *vid = advGroup->addSection(tr("Video recording"), Icon::Video, false);
    auto *vf = new QFormLayout;
    vf->setContentsMargins(0, 0, 0, 0);
    m_videoFps = new QSpinBox(this);
    m_videoFps->setRange(1, 60);
    m_videoFps->setSuffix(tr(" fps"));
    vf->addRow(tr("Frame rate"), m_videoFps);
    vid->contentLayout()->addLayout(vf);
    m_videoScale = new QCheckBox(tr("Include scale bar"), this);
    vid->contentLayout()->addWidget(m_videoScale);
    m_record = new QPushButton(tr("Record video"), this);
    m_record->setIcon(icon(Icon::Video, theme().live, 15));
    m_record->setCheckable(true);
    m_record->setObjectName(QStringLiteral("LiveButton"));
    vid->contentLayout()->addWidget(m_record);
    m_recStatus = new QLabel(this);
    m_recStatus->setObjectName(QStringLiteral("Hint"));
    m_recStatus->setWordWrap(true);
    vid->contentLayout()->addWidget(m_recStatus);

    // --- time lapse
    auto *tl = advGroup->addSection(tr("Time lapse"), Icon::Timelapse, false);
    auto *tf = new QFormLayout;
    tf->setContentsMargins(0, 0, 0, 0);
    m_tlInterval = new QDoubleSpinBox(this);
    m_tlInterval->setRange(0.5, 86400);
    m_tlInterval->setDecimals(1);
    m_tlInterval->setSuffix(tr(" s"));
    tf->addRow(tr("Interval"), m_tlInterval);
    m_tlCount = new QSpinBox(this);
    m_tlCount->setRange(1, 100000);
    m_tlCount->setSuffix(tr(" images"));
    tf->addRow(tr("Count"), m_tlCount);
    tl->contentLayout()->addLayout(tf);
    m_tlStart = new QPushButton(tr("Start time lapse"), this);
    m_tlStart->setIcon(icon(Icon::Timelapse, theme().text, 15));
    m_tlStart->setCheckable(true);
    tl->contentLayout()->addWidget(m_tlStart);
    m_tlStatus = new QLabel(this);
    m_tlStatus->setObjectName(QStringLiteral("Hint"));
    tl->contentLayout()->addWidget(m_tlStatus);

    refreshFromSettings();
    setMultifocusRunning(false, 0);
    setMosaicRunning(false, 0, false);

    connect(m_capture, &QPushButton::clicked, this, &CapturePanel::captureRequested);
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Output folder"), m_folder->text());
        if (!d.isEmpty()) {
            m_folder->setText(d);
            store();
        }
    });
    connect(open, &QToolButton::clicked, this, [this] {
        QDir().mkpath(m_folder->text());
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_folder->text()));
    });
    for (QLineEdit *e : {m_folder, m_sample, m_pattern})
        connect(e, &QLineEdit::editingFinished, this, &CapturePanel::store);
    connect(m_counter, &QSpinBox::valueChanged, this, &CapturePanel::store);
    connect(m_format, &QComboBox::activated, this, &CapturePanel::store);
    connect(m_jpegQuality, &QSpinBox::valueChanged, this, &CapturePanel::store);
    connect(m_average, &QSpinBox::valueChanged, this, &CapturePanel::store);
    connect(m_mode, &QComboBox::activated, this, &CapturePanel::store);
    connect(m_burnScale, &QCheckBox::toggled, this, &CapturePanel::store);
    connect(m_openProcess, &QCheckBox::toggled, this, &CapturePanel::store);
    connect(m_prompt, &QCheckBox::toggled, this, &CapturePanel::store);
    connect(m_tlInterval, &QDoubleSpinBox::valueChanged, this, &CapturePanel::store);
    connect(m_tlCount, &QSpinBox::valueChanged, this, &CapturePanel::store);
    connect(m_tlStart, &QPushButton::toggled, this, &CapturePanel::timelapseToggled);
    connect(m_videoFps, &QSpinBox::valueChanged, this, &CapturePanel::store);
    connect(m_videoScale, &QCheckBox::toggled, this, &CapturePanel::store);
    connect(m_record, &QPushButton::toggled, this, &CapturePanel::recordToggled);
    connect(m_mfStart, &QPushButton::clicked, this, &CapturePanel::multifocusStart);
    connect(m_mfFinish, &QPushButton::clicked, this, &CapturePanel::multifocusFinish);
    connect(m_mfCancel, &QPushButton::clicked, this, &CapturePanel::multifocusCancel);
    connect(m_moStart, &QPushButton::clicked, this, &CapturePanel::mosaicStart);
    connect(m_moFinish, &QPushButton::clicked, this, &CapturePanel::mosaicFinish);
    connect(m_moCancel, &QPushButton::clicked, this, &CapturePanel::mosaicCancel);
    connect(m_moAdd, &QPushButton::clicked, this, &CapturePanel::mosaicAddTile);
    connect(m_moAuto, &QCheckBox::toggled, this, &CapturePanel::mosaicAutoAddChanged);
}

void CapturePanel::setShotModes(const QStringList &names, bool hdrAvailable)
{
    m_updating = true;
    m_mode->clear();
    m_mode->addItem(tr("Standard (single shot)"), -1);
    if (hdrAvailable) {
        const struct {
            int exposures;
            QString label, tip;
        } hdr[] = {
            {2, tr("HDR, 2 exposures (dark stains, less noise)"),
             tr("Two exposures, 4× apart, merged: dark areas get 4× the signal while the background stays "
                "unclipped. Set the exposure for the background first (auto exposure).")},
            {3, tr("HDR, 3 exposures (very dark stains)"),
             tr("Three exposures (1×, 4×, 16×) merged into one 16-bit image. Takes longer; the specimen must "
                "not move.")},
        };
        for (const auto &h : hdr) {
            m_mode->addItem(h.label, hdrShotMode(h.exposures));
            m_mode->setItemData(m_mode->count() - 1, h.tip, Qt::ToolTipRole);
        }
    }
    for (int i = 0; i < names.size(); ++i)
        m_mode->addItem(names[i], i);
    const int want = AppSettings::instance().capture.shotMode;
    const int idx = m_mode->findData(want);
    m_mode->setCurrentIndex(idx >= 0 ? idx : 0);
    m_updating = false;
}

void CapturePanel::refreshFromSettings()
{
    m_updating = true;
    const auto &c = AppSettings::instance().capture;
    m_folder->setText(c.folder);
    m_sample->setText(c.sample);
    m_pattern->setText(c.pattern);
    m_counter->setValue(c.counter);
    int fi = 0;
    for (int i = 0; i < int(std::size(kFormats)); ++i)
        if (kFormats[i].fmt == c.save.format && kFormats[i].sixteen == c.save.sixteenBit) {
            fi = i;
            break;
        }
    m_format->setCurrentIndex(fi);
    m_jpegQuality->setValue(c.save.jpegQuality);
    m_average->setValue(c.averageFrames);
    m_burnScale->setChecked(c.burnScaleBar);
    m_openProcess->setChecked(c.openInProcess);
    m_prompt->setChecked(c.promptAfterCapture);
    m_tlInterval->setValue(c.timelapseIntervalS);
    m_tlCount->setValue(c.timelapseCount);
    m_videoFps->setValue(c.videoFps);
    m_videoScale->setChecked(c.videoScaleBar);
    m_updating = false;
}

void CapturePanel::store()
{
    if (m_updating)
        return;
    auto &c = AppSettings::instance().capture;
    c.folder = m_folder->text();
    c.sample = m_sample->text();
    c.pattern = m_pattern->text();
    c.counter = m_counter->value();
    const auto &f = kFormats[std::clamp(m_format->currentIndex(), 0, int(std::size(kFormats)) - 1)];
    c.save.format = f.fmt;
    c.save.sixteenBit = f.sixteen;
    c.save.jpegQuality = m_jpegQuality->value();
    c.averageFrames = m_average->value();
    c.shotMode = m_mode->currentData().toInt();
    c.burnScaleBar = m_burnScale->isChecked();
    c.openInProcess = m_openProcess->isChecked();
    c.promptAfterCapture = m_prompt->isChecked();
    c.timelapseIntervalS = m_tlInterval->value();
    c.timelapseCount = m_tlCount->value();
    c.videoFps = m_videoFps->value();
    c.videoScaleBar = m_videoScale->isChecked();
    m_jpegQuality->setEnabled(f.fmt == FileFormat::Jpeg);
    m_average->setEnabled(c.shotMode < 0);
    AppSettings::instance().save();
    emit settingsChanged();
}

void CapturePanel::updateNextName(const QString &name)
{
    m_nextName->setText(tr("Next: %1").arg(name));
    QSignalBlocker b(m_counter);
    m_counter->setValue(AppSettings::instance().capture.counter);
}

void CapturePanel::setBusy(bool busy, const QString &what)
{
    m_capture->setEnabled(!busy);
    m_capture->setText(busy ? (what.isEmpty() ? tr("Capturing…") : what) : tr("Capture image"));
    m_progress->setVisible(busy);
    if (busy)
        m_progress->setRange(0, 0);
}

void CapturePanel::setProgress(int done, int total)
{
    m_progress->setVisible(true);
    m_progress->setRange(0, total);
    m_progress->setValue(done);
}

void CapturePanel::setTimelapseRunning(bool on, int done, int total)
{
    QSignalBlocker b(m_tlStart);
    m_tlStart->setChecked(on);
    m_tlStart->setText(on ? tr("Stop time lapse") : tr("Start time lapse"));
    m_tlStatus->setText(on || done > 0 ? tr("%1 of %2 images").arg(done).arg(total) : QString());
}

void CapturePanel::setRecording(bool on, const QString &status)
{
    QSignalBlocker b(m_record);
    m_record->setChecked(on);
    m_record->setText(on ? tr("■ Stop recording") : tr("● Record video"));
    m_recStatus->setText(status);
    m_videoFps->setEnabled(!on);
}

void CapturePanel::setMultifocusRunning(bool on, int frames)
{
    m_mfStart->setEnabled(!on);
    m_mfFinish->setEnabled(on && frames > 0);
    m_mfCancel->setEnabled(on);
    m_mfStatus->setText(on ? tr("%1 frames merged").arg(frames) : QString());
}

void CapturePanel::setMosaicRunning(bool on, int tiles, bool tracking)
{
    m_moStart->setEnabled(!on);
    m_moFinish->setEnabled(on && tiles > 0);
    m_moCancel->setEnabled(on);
    m_moAdd->setEnabled(on);
    m_moStatus->setText(on ? tr("%1 tiles — %2").arg(tiles).arg(tracking ? tr("tracking") : tr("position lost: move back to the mosaic"))
                           : QString());
}

} // namespace lm
