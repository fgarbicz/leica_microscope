#include "CalibrationRepairDialog.h"

#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QSettings>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace lm {

CalibrationRepairDialog::CalibrationRepairDialog(const QString &folder, double wrongAdapter, double rightAdapter,
                                                 QWidget *parent)
    : QDialog(parent), m_wrongAdapter(wrongAdapter), m_rightAdapter(rightAdapter)
{
    // the sensor pitch the nominal pixel sizes were computed from (MicroscopeConfig's setting)
    m_sensorPixelUm = QSettings().value(QStringLiteral("microscope/sensorPixel"), kNominalSensorPixelUm).toDouble();

    setWindowTitle(tr("Correct the pixel size of saved images"));
    resize(px(820), px(560));
    auto *lay = new QVBoxLayout(this);

    auto *intro = new QLabel(
        tr("<p>Images saved with the nominal pixel size while the camera adapter was recorded as <b>%1×</b> "
           "carry a pixel size that is <b>%2× too large</b>. The adapter on this microscope is <b>%3×</b>, which "
           "is what LAS X uses for the same objectives.</p>"
           "<p>This rewrites the scale recorded in each file — the TIFF resolution tags, the pixel density and "
           "the metadata — so ImageJ, QuPath and this program all read the right size. Images calibrated with a "
           "stage micrometer or set by hand were right and are listed, but left alone. <b>The image itself is "
           "not touched</b>, and nothing is changed until you press the button below.</p>")
            .arg(wrongAdapter, 0, 'g', 3)
            .arg(rightAdapter / wrongAdapter, 0, 'g', 3)
            .arg(rightAdapter, 0, 'g', 3),
        this);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Folder"), this));
    m_folder = new QLineEdit(folder, this);
    row->addWidget(m_folder, 1);
    m_browse = new QToolButton(this);
    m_browse->setText(QStringLiteral("…"));
    row->addWidget(m_browse);
    lay->addLayout(row);

    m_recursive = new QCheckBox(tr("Include folders inside it"), this);
    m_recursive->setChecked(true);
    lay->addWidget(m_recursive);

    auto *scanRow = new QHBoxLayout;
    m_scan = new QPushButton(icon(Icon::Refresh, theme().text, 15), tr("Scan"), this);
    scanRow->addWidget(m_scan);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    scanRow->addWidget(m_summary, 1);
    lay->addLayout(scanRow);

    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels({tr("Image"), tr("Recorded µm/pixel"), tr("Corrected µm/pixel"), tr("Note")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    // the numbers' columns fit their titles, which the default width cut off
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->setVisible(false);
    lay->addWidget(m_table, 1);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    lay->addWidget(m_progress);

    auto *bb = new QDialogButtonBox(this);
    // ActionRole, not AcceptRole: a button box makes its first AcceptRole button
    // the default when it is shown, whatever was set here, and Enter then
    // pressed it right after scanning
    m_apply = bb->addButton(tr("Correct these images"), QDialogButtonBox::ActionRole);
    m_apply->setEnabled(false);
    m_close = bb->addButton(QDialogButtonBox::Close);
    lay->addWidget(bb);
    // Enter must never rewrite files: no button is the default, and Enter in the
    // folder field scans that folder instead
    for (QPushButton *b : {m_scan, m_apply, m_close}) {
        b->setAutoDefault(false);
        b->setDefault(false);
    }

    connect(m_browse, &QToolButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Folder with saved images"), m_folder->text());
        if (!d.isEmpty()) {
            m_folder->setText(d);
            scan();
        }
    });
    connect(m_folder, &QLineEdit::textEdited, this, &CalibrationRepairDialog::clearList);
    connect(m_folder, &QLineEdit::returnPressed, this, &CalibrationRepairDialog::scan);
    connect(m_recursive, &QCheckBox::toggled, this, &CalibrationRepairDialog::clearList);
    connect(m_scan, &QPushButton::clicked, this, &CalibrationRepairDialog::scan);
    connect(m_apply, &QPushButton::clicked, this, &CalibrationRepairDialog::apply);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    scan();
}

void CalibrationRepairDialog::reject()
{
    if (!m_busy)
        QDialog::reject();
}

void CalibrationRepairDialog::setBusy(bool busy)
{
    m_busy = busy;
    for (QWidget *w : std::initializer_list<QWidget *>{m_folder, m_browse, m_recursive, m_scan, m_close})
        w->setEnabled(!busy);
    m_apply->setEnabled(!busy && !m_fixes.isEmpty());
}

void CalibrationRepairDialog::clearList()
{
    m_fixes.clear();
    m_leftAlone.clear();
    m_scannedFolder.clear();
    m_table->setRowCount(0);
    m_apply->setEnabled(false);
    m_summary->setText(tr("Press Scan (or Enter) to look for images in this folder."));
    m_summary->setObjectName(QStringLiteral("Hint"));
    m_summary->style()->unpolish(m_summary);
    m_summary->style()->polish(m_summary);
}

void CalibrationRepairDialog::scan()
{
    if (m_busy)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_scannedFolder = m_folder->text();
    m_fixes = findCalibrationFixes(m_scannedFolder, m_recursive->isChecked(), m_wrongAdapter, m_rightAdapter,
                                   &m_leftAlone, m_sensorPixelUm);
    QApplication::restoreOverrideCursor();

    m_table->setRowCount(int(m_fixes.size() + m_leftAlone.size()));
    int jpegOnlySidecar = 0;
    int r = 0;
    for (const CalibrationFix &f : std::as_const(m_fixes)) {
        m_table->setItem(r, 0, new QTableWidgetItem(QFileInfo(f.path).fileName()));
        m_table->item(r, 0)->setToolTip(f.path);
        m_table->setItem(r, 1, new QTableWidgetItem(QString::number(f.oldUmPerPixel, 'g', 5)));
        m_table->setItem(r, 2, new QTableWidgetItem(QString::number(f.newUmPerPixel, 'g', 5)));
        QString note;
        if (!f.rewritesFile) {
            ++jpegOnlySidecar;
            note = f.createsSidecar ? tr("JPEG: a metadata file is created beside it")
                                    : tr("JPEG: the metadata file beside it is corrected");
        }
        m_table->setItem(r, 3, new QTableWidgetItem(note));
        ++r;
    }
    // listed so they are seen, never applied
    for (const CalibrationFix &f : std::as_const(m_leftAlone)) {
        const QColor dim = theme().subText;
        const QStringList cells = {QFileInfo(f.path).fileName(), QString::number(f.oldUmPerPixel, 'g', 5),
                                   QStringLiteral("—"), f.note};
        for (int c = 0; c < cells.size(); ++c) {
            auto *item = new QTableWidgetItem(cells[c]);
            item->setForeground(dim);
            item->setToolTip(c == 0 ? f.path : f.note);
            m_table->setItem(r, c, item);
        }
        ++r;
    }
    m_apply->setEnabled(!m_fixes.isEmpty());

    QString t;
    if (m_fixes.isEmpty()) {
        t = tr("No images here need correcting.");
        m_summary->setObjectName(QStringLiteral("Hint"));
    } else {
        t = tr("%n image(s) to correct.", nullptr, int(m_fixes.size()));
        if (jpegOnlySidecar > 0)
            t += QLatin1Char(' ')
                 + tr("%n of them are JPEG: changing the density inside a JPEG means re-encoding it and losing "
                      "quality, so only the .json metadata file beside it is corrected (or created). This program "
                      "reads the corrected size; other programs still see the old one.",
                      nullptr, jpegOnlySidecar);
        m_summary->setObjectName(QStringLiteral("StatusWarn"));
    }
    if (!m_leftAlone.isEmpty())
        t += QLatin1Char(' ')
             + tr("%n image(s) record the %1× adapter but cannot be shown to use the nominal pixel size, "
                  "and are left alone; the list gives the reason for each.",
                  nullptr, int(m_leftAlone.size()))
                   .arg(m_wrongAdapter, 0, 'g', 3);
    m_summary->setText(t);
    m_summary->style()->unpolish(m_summary);
    m_summary->style()->polish(m_summary);
}

void CalibrationRepairDialog::apply()
{
    if (m_fixes.isEmpty() || m_busy)
        return;
    const QString folder = QDir::toNativeSeparators(m_scannedFolder);
    if (QMessageBox::question(this, tr("Correct the pixel size"),
                              tr("Rewrite the recorded scale of %n image(s) in\n%1?\n\nThe images themselves are not "
                                 "changed, but the files are replaced, so make sure nothing else has them open.",
                                 nullptr, int(m_fixes.size()))
                                  .arg(folder),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;

    setBusy(true);
    m_progress->setVisible(true);
    m_progress->setRange(0, int(m_fixes.size()));
    int done = 0;
    QStringList failed;
    for (int i = 0; i < m_fixes.size(); ++i) {
        QString err;
        if (applyCalibrationFix(m_fixes[i], m_rightAdapter, &err))
            ++done;
        else
            failed << tr("%1 (%2)").arg(QFileInfo(m_fixes[i].path).fileName(), err);
        m_progress->setValue(i + 1);
        QApplication::processEvents();
    }
    m_progress->setVisible(false);
    setBusy(false);

    if (failed.isEmpty()) {
        QMessageBox::information(this, tr("Correct the pixel size"),
                                 tr("%n image(s) now record the right pixel size.", nullptr, done));
    } else {
        QMessageBox box(QMessageBox::Warning, tr("Correct the pixel size"),
                        tr("%1 of %2 images were corrected. The rest were left as they were.")
                            .arg(done)
                            .arg(m_fixes.size()),
                        QMessageBox::Ok, this);
        box.setDetailedText(failed.join(QLatin1Char('\n')));
        box.exec();
    }
    scan(); // what is left (the folder field was locked, so it is the same folder)
}

} // namespace lm
