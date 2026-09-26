#include "CalibrationRepairDialog.h"

#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace lm {

CalibrationRepairDialog::CalibrationRepairDialog(const QString &folder, double wrongAdapter, double rightAdapter,
                                                 QWidget *parent)
    : QDialog(parent), m_wrongAdapter(wrongAdapter), m_rightAdapter(rightAdapter)
{
    setWindowTitle(tr("Correct the pixel size of saved images"));
    resize(px(760), px(560));
    auto *lay = new QVBoxLayout(this);

    auto *intro = new QLabel(
        tr("<p>Images saved while the camera adapter was recorded as <b>%1×</b> carry a pixel size that is "
           "<b>%2× too large</b>. The adapter on this microscope is <b>%3×</b>, which is what LAS X uses for the "
           "same objectives.</p>"
           "<p>This rewrites the scale recorded in each file — the TIFF resolution tags, the pixel density and "
           "the metadata — so ImageJ, QuPath and this program all read the right size. <b>The image itself is "
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
    auto *browse = new QToolButton(this);
    browse->setText(QStringLiteral("…"));
    row->addWidget(browse);
    lay->addLayout(row);

    m_recursive = new QCheckBox(tr("Include folders inside it"), this);
    m_recursive->setChecked(true);
    lay->addWidget(m_recursive);

    auto *scanRow = new QHBoxLayout;
    auto *scanBtn = new QPushButton(icon(Icon::Refresh, theme().text, 15), tr("Scan"), this);
    scanRow->addWidget(scanBtn);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    scanRow->addWidget(m_summary, 1);
    lay->addLayout(scanRow);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({tr("Image"), tr("Recorded µm/pixel"), tr("Corrected µm/pixel")});
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->setVisible(false);
    lay->addWidget(m_table, 1);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    lay->addWidget(m_progress);

    auto *bb = new QDialogButtonBox(this);
    m_apply = bb->addButton(tr("Correct these images"), QDialogButtonBox::AcceptRole);
    m_apply->setEnabled(false);
    bb->addButton(QDialogButtonBox::Close);
    lay->addWidget(bb);

    connect(browse, &QToolButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Folder with saved images"), m_folder->text());
        if (!d.isEmpty()) {
            m_folder->setText(d);
            scan();
        }
    });
    connect(scanBtn, &QPushButton::clicked, this, &CalibrationRepairDialog::scan);
    connect(m_apply, &QPushButton::clicked, this, &CalibrationRepairDialog::apply);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    scan();
}

void CalibrationRepairDialog::scan()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_fixes = findCalibrationFixes(m_folder->text(), m_recursive->isChecked(), m_wrongAdapter, m_rightAdapter);
    QApplication::restoreOverrideCursor();

    m_table->setRowCount(int(m_fixes.size()));
    int jpegOnlySidecar = 0;
    for (int i = 0; i < m_fixes.size(); ++i) {
        const CalibrationFix &f = m_fixes[i];
        m_table->setItem(i, 0, new QTableWidgetItem(QFileInfo(f.path).fileName()));
        m_table->item(i, 0)->setToolTip(f.path);
        m_table->setItem(i, 1, new QTableWidgetItem(QString::number(f.oldUmPerPixel, 'g', 5)));
        m_table->setItem(i, 2, new QTableWidgetItem(QString::number(f.newUmPerPixel, 'g', 5)));
        if (!f.rewritesFile)
            ++jpegOnlySidecar;
    }
    m_apply->setEnabled(!m_fixes.isEmpty());
    if (m_fixes.isEmpty()) {
        m_summary->setText(tr("No images here were saved with the %1× adapter.").arg(m_wrongAdapter, 0, 'g', 3));
        m_summary->setObjectName(QStringLiteral("Hint"));
    } else {
        QString t = tr("%n image(s) to correct.", nullptr, int(m_fixes.size()));
        if (jpegOnlySidecar > 0)
            t += QLatin1Char(' ')
                 + tr("%n of them are JPEG: only the metadata beside them is corrected, because changing the "
                      "density inside a JPEG means re-encoding it and losing quality.",
                      nullptr, jpegOnlySidecar);
        m_summary->setText(t);
        m_summary->setObjectName(QStringLiteral("StatusWarn"));
    }
    m_summary->style()->unpolish(m_summary);
    m_summary->style()->polish(m_summary);
}

void CalibrationRepairDialog::apply()
{
    if (m_fixes.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Correct the pixel size"),
                              tr("Rewrite the recorded scale of %n image(s)?\n\nThe images themselves are not "
                                 "changed, but the files are replaced, so make sure nothing else has them open.",
                                 nullptr, int(m_fixes.size())),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;

    m_progress->setVisible(true);
    m_progress->setRange(0, int(m_fixes.size()));
    m_apply->setEnabled(false);
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
    scan(); // what is left
}

} // namespace lm
