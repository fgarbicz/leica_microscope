#include "CaptureDialog.h"

#include "app/Calibration.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

namespace lm {

CaptureDialog::CaptureDialog(const QImage &preview, const MicroscopeConfig &scope, int objective,
                             std::function<QString(int)> nameForObjective, const QString &info, QWidget *parent)
    : QDialog(parent), m_nameFor(std::move(nameForObjective))
{
    setWindowTitle(tr("Save image"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    lay->setSpacing(10);

    auto *pic = new QLabel(this);
    pic->setAlignment(Qt::AlignCenter);
    pic->setPixmap(QPixmap::fromImage(preview.scaled(560, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    lay->addWidget(pic);
    if (!info.isEmpty()) {
        auto *inf = new QLabel(info, this);
        inf->setObjectName(QStringLiteral("Hint"));
        inf->setAlignment(Qt::AlignCenter);
        lay->addWidget(inf);
    }

    auto *magLabel = new QLabel(tr("Objective magnification"), this);
    magLabel->setStyleSheet(QStringLiteral("font-weight:600;"));
    lay->addWidget(magLabel);
    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    for (int i = 0; i < scope.objectives.size(); ++i) {
        const Objective &o = scope.objectives[i];
        auto *b = new QPushButton(QStringLiteral("%1×").arg(o.magnification), this);
        b->setCheckable(true);
        b->setMinimumSize(72, 44);
        b->setToolTip(QStringLiteral("%1   (key %2)").arg(scope.objectiveLabel(o)).arg(i + 1));
        b->setStyleSheet(QStringLiteral("QPushButton{font-size:12pt;font-weight:700;}"));
        m_group->addButton(b, i);
        row->addWidget(b);
    }
    lay->addLayout(row);

    auto *form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_name->setMinimumWidth(420);
    // no characters that are invalid in Windows file names
    m_name->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[^\\\\/:*?\"<>|]*")), m_name));
    form->addRow(tr("Image name"), m_name);
    m_notes = new QLineEdit(this);
    m_notes->setPlaceholderText(tr("optional (stored in the image metadata)"));
    form->addRow(tr("Notes"), m_notes);
    lay->addLayout(form);

    auto *bb = new QDialogButtonBox(this);
    auto *save = bb->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    save->setDefault(true);
    save->setObjectName(QStringLiteral("PrimaryButton"));
    bb->addButton(tr("Discard"), QDialogButtonBox::RejectRole);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (!m_name->text().trimmed().isEmpty())
            accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_group, &QButtonGroup::idClicked, this, &CaptureDialog::onObjectiveChanged);

    if (QAbstractButton *b = m_group->button(objective))
        b->setChecked(true);
    m_autoName = m_nameFor ? m_nameFor(objective) : QString();
    m_name->setText(m_autoName);
    m_name->selectAll();
    m_name->setFocus();
    m_name->installEventFilter(this);
}

void CaptureDialog::onObjectiveChanged(int index)
{
    // keep the suggested name in sync with the objective until the user edits it
    if (m_nameFor && m_name->text() == m_autoName) {
        m_autoName = m_nameFor(index);
        m_name->setText(m_autoName);
        m_name->selectAll();
    }
    m_name->setFocus();
}

bool CaptureDialog::eventFilter(QObject *o, QEvent *e)
{
    // keys 1..6 choose the objective while the suggested name is untouched
    // (Alt+1..6 at any time); otherwise digits go into the name
    if (o == m_name && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        const int k = ke->key() - Qt::Key_1;
        const bool alt = ke->modifiers() & Qt::AltModifier;
        if (k >= 0 && k < m_group->buttons().size() && (alt || m_name->text() == m_autoName)) {
            m_group->button(k)->setChecked(true);
            onObjectiveChanged(k);
            return true;
        }
    }
    return QDialog::eventFilter(o, e);
}

int CaptureDialog::objectiveIndex() const
{
    return m_group->checkedId();
}

QString CaptureDialog::imageName() const
{
    return m_name->text().trimmed();
}

QString CaptureDialog::notes() const
{
    return m_notes->text().trimmed();
}

} // namespace lm
