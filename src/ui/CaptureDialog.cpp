#include "ui/Theme.h"
#include "CaptureDialog.h"

#include "ui/Icons.h"

#include "app/Calibration.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QToolButton>
#include <QVBoxLayout>

namespace lm {

CaptureDialog::CaptureDialog(const QImage &preview, const MicroscopeConfig &scope, int objective,
                             std::function<QString(int)> nameForObjective, const QString &info,
                             const QString &folder, QWidget *parent)
    : QDialog(parent), m_nameFor(std::move(nameForObjective)), m_folder(folder)
{
    setWindowTitle(tr("Save image"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    lay->setSpacing(px(10));

    auto *pic = new QLabel(this);
    pic->setAlignment(Qt::AlignCenter);
    {
        // at the interface size, and in physical pixels on a high-DPI screen
        const qreal dpr = devicePixelRatioF();
        QPixmap pm = QPixmap::fromImage(
            preview.scaled(QSize(px(560), px(360)) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        pm.setDevicePixelRatio(dpr);
        pic->setPixmap(pm);
    }
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
    row->setSpacing(px(6));
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    for (int i = 0; i < scope.objectives.size(); ++i) {
        const Objective &o = scope.objectives[i];
        auto *b = new QPushButton(QStringLiteral("%1×").arg(o.magnification), this);
        b->setCheckable(true);
        b->setMinimumSize(px(72), px(44));
        b->setToolTip(QStringLiteral("%1   (key %2)").arg(scope.objectiveLabel(o)).arg(i + 1));
        // a third larger than the interface text, so it follows the interface size
        b->setStyleSheet(QStringLiteral("QPushButton{font-size:%1pt;font-weight:700;}")
                             .arg(QApplication::font().pointSizeF() * 4 / 3, 0, 'f', 1));
        m_group->addButton(b, i);
        row->addWidget(b);
    }
    lay->addLayout(row);

    auto *form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_name->setMinimumWidth(px(420));
    // no characters that are invalid in Windows file names
    m_name->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[^\\\\/:*?\"<>|]*")), m_name));
    form->addRow(tr("Image name"), m_name);
    m_notes = new QLineEdit(this);
    m_notes->setPlaceholderText(tr("optional (stored in the image metadata)"));
    form->addRow(tr("Notes"), m_notes);
    // where the image goes, changeable here; the choice becomes the image folder
    auto *folderRow = new QHBoxLayout;
    folderRow->setSpacing(px(6));
    m_folderLabel = new QLabel(this);
    m_folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    folderRow->addWidget(m_folderLabel, 1);
    auto *choose = new QToolButton(this);
    choose->setIcon(icon(Icon::Folder));
    choose->setText(tr("Change…"));
    choose->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    choose->setToolTip(tr("Choose the folder this image (and the next ones) are saved in"));
    connect(choose, &QToolButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Save images in"), m_folder);
        if (!d.isEmpty()) {
            m_folder = d;
            showFolder();
        }
        m_name->setFocus();
    });
    folderRow->addWidget(choose);
    form->addRow(tr("Save in"), folderRow);
    lay->addLayout(form);
    showFolder();

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
    // an image needs a name: Save is only available with one
    connect(m_name, &QLineEdit::textChanged, save,
            [save](const QString &t) { save->setEnabled(!t.trimmed().isEmpty()); });
    save->setToolTip(tr("Save the image under this name (Enter)"));
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

void CaptureDialog::reject()
{
    // Esc, the window's close button and "Discard" all end up here: the image
    // (maybe a long pixel-shift capture) is lost unless the user confirms
    QMessageBox box(QMessageBox::Question, tr("Discard image"), tr("Discard this image? It has not been saved."),
                    QMessageBox::NoButton, this);
    auto *discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    auto *keep = box.addButton(tr("Keep editing"), QMessageBox::RejectRole);
    box.setDefaultButton(keep);
    box.exec();
    if (box.clickedButton() == discard)
        QDialog::reject();
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

void CaptureDialog::showFolder()
{
    const QString native = QDir::toNativeSeparators(m_folder);
    // long paths keep their end (the folder's own name) visible
    m_folderLabel->setText(m_folderLabel->fontMetrics().elidedText(native, Qt::ElideLeft, px(380)));
    m_folderLabel->setToolTip(native);
}

QString CaptureDialog::notes() const
{
    return m_notes->text().trimmed();
}

} // namespace lm
