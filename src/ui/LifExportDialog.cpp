#include "LifExportDialog.h"

#include "app/AppSettings.h"
#include "ui/Overlays.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace lm {

namespace {
const QString kKey = QStringLiteral("lifExport/");
}

LifExportDialog::LifExportDialog(const LifFileIndex &index, const LifExportItem &current, const QList<int> &selected,
                                 const QHash<int, QList<LifChannelDisplay>> &displays, QWidget *parent)
    : QDialog(parent), m_index(index), m_current(current), m_selected(selected), m_displays(displays)
{
    setWindowTitle(tr("Export from %1").arg(QFileInfo(index.path).fileName()));
    const QSettings s;
    auto *lay = new QVBoxLayout(this);
    // never smaller than its contents: the wrapped hints need their lines
    lay->setSizeConstraint(QLayout::SetMinimumSize);
    setMinimumWidth(px(620));

    // ---- which images
    auto *which = new QGroupBox(tr("Images"), this);
    auto *wl = new QVBoxLayout(which);
    const QString currentName =
        current.image >= 0 && current.image < index.images.size() ? index.images[current.image].name : QString();
    m_scope[CurrentImage] = new QRadioButton(tr("This image: %1").arg(currentName), which);
    m_scope[SelectedImages] = new QRadioButton(tr("The images selected in the list (%1)").arg(selected.size()), which);
    m_scope[AllImages] = new QRadioButton(tr("Every image in the file (%1)").arg(index.images.size()), which);
    m_scope[SelectedImages]->setEnabled(!selected.isEmpty());
    auto *scopeGroup = new QButtonGroup(this);
    for (int i = 0; i < 3; ++i) {
        wl->addWidget(m_scope[i]);
        scopeGroup->addButton(m_scope[i], i);
    }
    m_scope[CurrentImage]->setChecked(true);
    lay->addWidget(which);

    // ---- as what
    auto *kind = new QGroupBox(tr("Export as"), this);
    auto *kl = new QVBoxLayout(kind);
    m_original = new QRadioButton(tr("Original data: ImageJ TIFF"), kind);
    auto *origHint = new QLabel(tr("Every channel, z slice and time point, unscaled (8- or 16-bit), calibrated, the "
                                   "channel colours as LUTs. Opens as a hyperstack in ImageJ/Fiji, and in QuPath, "
                                   "napari or Python. For measuring."),
                                kind);
    origHint->setObjectName(QStringLiteral("Hint"));
    origHint->setWordWrap(true);
    origHint->setContentsMargins(px(22), 0, 0, px(4));
    m_pictures = new QRadioButton(tr("Pictures as shown"), kind);
    auto *picHint = new QLabel(tr("A colour picture with the channels, colours and contrast set in the viewer. "
                                  "For slides and reports."),
                               kind);
    picHint->setObjectName(QStringLiteral("Hint"));
    picHint->setWordWrap(true);
    picHint->setContentsMargins(px(22), 0, 0, px(4));
    auto *kindGroup = new QButtonGroup(this);
    kindGroup->addButton(m_original);
    kindGroup->addButton(m_pictures);
    // a wrapped label inside a group box does not reserve its lines by itself
    for (QLabel *hint : {origHint, picHint})
        hint->setMinimumHeight(hint->heightForWidth(px(560)));
    kl->addWidget(m_original);
    kl->addWidget(origHint);
    kl->addWidget(m_pictures);
    kl->addWidget(picHint);
    auto *pf = new QFormLayout;
    pf->setContentsMargins(px(22), 0, 0, 0);
    m_planes = new QComboBox(kind);
    m_planes->addItem(tr("The plane shown"), int(LifExportOptions::CurrentPlane));
    m_planes->addItem(tr("Every plane (each z slice, time point and tile)"), int(LifExportOptions::AllPlanes));
    m_planes->addItem(tr("Maximum projection along z"), int(LifExportOptions::MaxProjection));
    m_planes->setToolTip(tr("Images not opened in the viewer are exported from their first plane"));
    m_format = new QComboBox(kind);
    m_format->addItem(tr("JPEG"), int(FileFormat::Jpeg));
    m_format->addItem(tr("PNG"), int(FileFormat::Png));
    m_format->addItem(tr("TIFF 8-bit"), 8);
    m_format->addItem(tr("TIFF 16-bit"), 16);
    m_scaleBar = new QCheckBox(tr("Burn in a scale bar"), kind);
    m_pictureLabel[0] = new QLabel(tr("Planes"), kind);
    m_pictureLabel[1] = new QLabel(tr("Format"), kind);
    pf->addRow(m_pictureLabel[0], m_planes);
    pf->addRow(m_pictureLabel[1], m_format);
    pf->addRow(QString(), m_scaleBar);
    kl->addLayout(pf);
    lay->addWidget(kind);

    // ---- where
    auto *where = new QGroupBox(tr("Output"), this);
    auto *ol = new QFormLayout(where);
    m_merge = new QCheckBox(tr("Merge the tiles of a tile scan (else one file per tile)"), where);
    bool anyTiles = false;
    for (const LifEntry &e : index.images)
        anyTiles = anyTiles || e.hasTiles();
    m_merge->setVisible(anyTiles);
    m_subfolder = new QCheckBox(tr("In a new folder named after the .lif file"), where);
    m_summary = new QCheckBox(tr("With a summary (images.csv) and the LAS X metadata (XML)"), where);
    auto *fr = new QHBoxLayout;
    m_folder = new QLineEdit(where);
    auto *browse = new QToolButton(where);
    browse->setText(QStringLiteral("…"));
    fr->addWidget(m_folder, 1);
    fr->addWidget(browse);
    ol->addRow(tr("Folder"), fr);
    ol->addRow(QString(), m_subfolder);
    ol->addRow(QString(), m_summary);
    ol->addRow(QString(), m_merge);
    lay->addWidget(where);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    lay->addWidget(m_progress);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("Hint"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::RichText);
    lay->addWidget(m_status);
    lay->addStretch();
    auto *bb = new QDialogButtonBox(this);
    m_start = bb->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Close);
    lay->addWidget(bb);

    // the last choices, and the folder of the .lif the first time
    (s.value(kKey + QStringLiteral("pictures"), false).toBool() ? m_pictures : m_original)->setChecked(true);
    m_planes->setCurrentIndex(std::clamp(s.value(kKey + QStringLiteral("planes"), 0).toInt(), 0, 2));
    m_format->setCurrentIndex(std::clamp(s.value(kKey + QStringLiteral("format"), 0).toInt(), 0, 3));
    m_scaleBar->setChecked(s.value(kKey + QStringLiteral("scaleBar"), true).toBool());
    m_merge->setChecked(s.value(kKey + QStringLiteral("merge"), true).toBool());
    m_subfolder->setChecked(s.value(kKey + QStringLiteral("subfolder"), true).toBool());
    m_summary->setChecked(s.value(kKey + QStringLiteral("summary"), true).toBool());
    QString folder = s.value(kKey + QStringLiteral("folder")).toString();
    if (folder.isEmpty() || !QDir(folder).exists())
        folder = QFileInfo(index.path).absolutePath();
    m_folder->setText(QDir::toNativeSeparators(folder));

    connect(browse, &QToolButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Export to"), m_folder->text());
        if (!d.isEmpty())
            m_folder->setText(QDir::toNativeSeparators(d));
    });
    connect(m_original, &QRadioButton::toggled, this, &LifExportDialog::updateControls);
    connect(m_format, &QComboBox::currentIndexChanged, this, &LifExportDialog::updateControls);
    connect(bb, &QDialogButtonBox::accepted, this, &LifExportDialog::run);
    connect(bb, &QDialogButtonBox::rejected, this, &LifExportDialog::reject);
    connect(m_status, &QLabel::linkActivated, this, [](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });
    updateControls();
}

void LifExportDialog::setScope(Scope s)
{
    if (m_scope[s]->isEnabled())
        m_scope[s]->setChecked(true);
}

void LifExportDialog::updateControls()
{
    const bool pictures = m_pictures->isChecked();
    for (QWidget *w : {static_cast<QWidget *>(m_planes), static_cast<QWidget *>(m_format),
                       static_cast<QWidget *>(m_scaleBar), static_cast<QWidget *>(m_pictureLabel[0]),
                       static_cast<QWidget *>(m_pictureLabel[1])})
        w->setEnabled(pictures && !m_running);
    // the scale bar is drawn into an 8-bit picture
    m_scaleBar->setEnabled(pictures && !m_running && m_format->currentData().toInt() != 16);
}

QList<LifExportItem> LifExportDialog::items() const
{
    QList<int> which;
    if (m_scope[CurrentImage]->isChecked())
        which << m_current.image;
    else if (m_scope[SelectedImages]->isChecked())
        which = m_selected;
    else
        for (int i = 0; i < m_index.images.size(); ++i)
            which << i;
    QList<LifExportItem> out;
    for (int i : which) {
        if (i == m_current.image) {
            out << m_current;
            continue;
        }
        LifExportItem it;
        it.image = i;
        it.display = m_displays.value(i); // as last shown, if it was
        out << it;
    }
    return out;
}

LifExportOptions LifExportDialog::options() const
{
    LifExportOptions o;
    o.kind = m_pictures->isChecked() ? LifExportOptions::AsShown : LifExportOptions::OriginalData;
    o.planes = LifExportOptions::Planes(m_planes->currentData().toInt());
    const int f = m_format->currentData().toInt();
    o.format = f == 8 || f == 16 ? FileFormat::Tiff : FileFormat(f);
    o.sixteenBit = f == 16;
    o.mergeTiles = m_merge->isChecked();
    o.subfolder = m_subfolder->isChecked();
    o.summary = m_summary->isChecked();
    if (o.kind == LifExportOptions::AsShown && m_scaleBar->isChecked() && !o.sixteenBit) {
        const OverlaySettings ov = AppSettings::instance().overlays;
        o.decorate = [ov](const QImage &img, double umPerPixel) {
            return umPerPixel > 0 ? burnScaleBar(img, umPerPixel, ov) : img;
        };
    }
    return o;
}

void LifExportDialog::reject()
{
    if (m_running) {
        *m_cancel = true;
        m_status->setText(tr("Cancelling…"));
        return;
    }
    QDialog::reject();
}

void LifExportDialog::run()
{
    if (m_running)
        return;
    const QString folder = QDir::fromNativeSeparators(m_folder->text().trimmed());
    if (folder.isEmpty() || !QDir().mkpath(folder)) {
        m_status->setText(tr("Cannot create the folder %1.").arg(m_folder->text().toHtmlEscaped()));
        return;
    }
    QSettings s;
    s.setValue(kKey + QStringLiteral("pictures"), m_pictures->isChecked());
    s.setValue(kKey + QStringLiteral("planes"), m_planes->currentIndex());
    s.setValue(kKey + QStringLiteral("format"), m_format->currentIndex());
    s.setValue(kKey + QStringLiteral("scaleBar"), m_scaleBar->isChecked());
    s.setValue(kKey + QStringLiteral("merge"), m_merge->isChecked());
    s.setValue(kKey + QStringLiteral("subfolder"), m_subfolder->isChecked());
    s.setValue(kKey + QStringLiteral("summary"), m_summary->isChecked());
    s.setValue(kKey + QStringLiteral("folder"), folder);

    m_running = true;
    *m_cancel = false;
    m_start->setEnabled(false);
    updateControls();
    m_progress->setRange(0, 0);
    m_status->setText(tr("Exporting…"));
    const LifFileIndex index = m_index; // the worker's own copy
    const QList<LifExportItem> list = items();
    const LifExportOptions opt = options();
    auto cancel = m_cancel;
    QPointer<LifExportDialog> self(this);
    QtConcurrent::run([index, list, opt, folder, cancel, self] {
        return exportLif(index, list, folder, opt, [cancel, self](qint64 done, qint64 total, const QString &what) {
            // progress goes to the dialog through its event loop
            QMetaObject::invokeMethod(
                qApp,
                [self, done, total, what] {
                    if (!self)
                        return;
                    self->m_progress->setRange(0, 1000);
                    self->m_progress->setValue(total > 0 ? int(1000 * done / total) : 0);
                    if (!*self->m_cancel)
                        self->m_status->setText(what.toHtmlEscaped());
                },
                Qt::QueuedConnection);
            return !cancel->load();
        });
    }).then(this, [this](const LifExportResult &r) {
        m_running = false;
        m_start->setEnabled(true);
        updateControls();
        m_progress->setRange(0, 1);
        m_progress->setValue(r.cancelled ? 0 : 1);
        QString text = r.cancelled ? tr("Cancelled after %1 files.").arg(r.written.size())
                                   : tr("%1 files written.").arg(r.written.size());
        if (!r.failed.isEmpty()) {
            QStringList shown = r.failed.mid(0, 5);
            for (QString &f : shown)
                f = f.toHtmlEscaped();
            text += QStringLiteral("<br>") + tr("%1 failed: %2").arg(r.failed.size()).arg(shown.join(QStringLiteral("<br>")));
        }
        text += QStringLiteral(" <a href=\"%1\">%2</a>").arg(QUrl::fromLocalFile(r.folder).toString(), tr("Open the folder"));
        m_status->setText(text);
    });
}

} // namespace lm
