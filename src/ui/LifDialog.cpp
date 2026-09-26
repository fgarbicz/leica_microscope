#include "LifDialog.h"

#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QVBoxLayout>

namespace lm {

namespace {
QString sizeText(const LifEntry &e)
{
    QString s = QStringLiteral("%1 x %2").arg(e.width).arg(e.height);
    if (e.umPerPixel > 0)
        s += LifDialog::tr(" · %1 µm/pixel").arg(e.umPerPixel, 0, 'g', 4);
    else
        s += LifDialog::tr(" · not calibrated");
    if (e.bitsPerSample != 8)
        s += QStringLiteral(" · %1 bit").arg(e.bitsPerSample);
    return s;
}
} // namespace

LifDialog::LifDialog(const QString &path, const QList<LifEntry> &entries, QWidget *parent)
    : QDialog(parent), m_path(path), m_entries(entries)
{
    setWindowTitle(tr("Open from %1").arg(QFileInfo(path).fileName()));
    resize(px(620), px(480));
    auto *lay = new QVBoxLayout(this);

    auto *head = new QLabel(tr("%n image(s) in this Leica file. Choose one to open.", nullptr, int(entries.size())), this);
    head->setWordWrap(true);
    lay->addWidget(head);

    m_list = new QListWidget(this);
    m_list->setIconSize(lm::iconSize(16));
    for (const LifEntry &e : m_entries) {
        auto *it = new QListWidgetItem(icon(Icon::Browse, theme().subText, 16),
                                       e.path.isEmpty() ? e.name : e.path + QStringLiteral(" / ") + e.name);
        it->setToolTip(sizeText(e));
        m_list->addItem(it);
    }
    m_list->setCurrentRow(0);
    lay->addWidget(m_list, 1);

    m_details = new QLabel(this);
    m_details->setObjectName(QStringLiteral("Hint"));
    m_details->setWordWrap(true);
    lay->addWidget(m_details);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            m_index = row;
        updateDetails();
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
    updateDetails();
}

void LifDialog::updateDetails()
{
    if (m_index < 0 || m_index >= m_entries.size()) {
        m_details->clear();
        return;
    }
    const LifEntry &e = m_entries[m_index];
    m_details->setText(sizeText(e));
}

bool LifDialog::openFrom(const QString &path, LoadedImage &out, QWidget *parent, QString *error)
{
    QList<LifEntry> entries;
    QString err;
    if (!readLifIndex(path, entries, &err)) {
        if (error)
            *error = err;
        return false;
    }
    LifEntry chosen = entries.first();
    if (entries.size() > 1) {
        LifDialog dlg(path, entries, parent);
        if (dlg.exec() != QDialog::Accepted) {
            if (error)
                error->clear(); // cancelled, not a failure
            return false;
        }
        chosen = dlg.selected();
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = readLifImage(path, chosen, out, &err);
    QApplication::restoreOverrideCursor();
    if (!ok && error)
        *error = err;
    return ok;
}

} // namespace lm
