#include "CompareWindow.h"

#include "app/AppSettings.h"
#include "io/ImageIO.h"
#include "ui/ImageView.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

namespace lm {

CompareWindow::CompareWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Compare images"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1600, 900);
    auto *root = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    m_sync = new QCheckBox(tr("Synchronise zoom && pan"), this);
    m_sync->setChecked(true);
    top->addWidget(m_sync);
    top->addStretch();
    root->addLayout(top);

    auto *split = new QSplitter(Qt::Horizontal, this);
    for (int i = 0; i < 2; ++i) {
        auto *pane = new QWidget(split);
        auto *pl = new QVBoxLayout(pane);
        pl->setContentsMargins(0, 0, 0, 0);
        auto *bar = new QHBoxLayout;
        m_label[i] = new QLabel(tr("(no image)"), pane);
        m_label[i]->setObjectName(QStringLiteral("Hint"));
        auto *open = new QPushButton(tr("Open…"), pane);
        bar->addWidget(m_label[i], 1);
        bar->addWidget(open);
        pl->addLayout(bar);
        m_view[i] = new ImageView(pane);
        m_view[i]->setPlaceholder(tr("Open an image"));
        pl->addWidget(m_view[i], 1);
        split->addWidget(pane);
        connect(open, &QPushButton::clicked, this, [this, i] {
            const QString f = QFileDialog::getOpenFileName(this, tr("Open image"), AppSettings::instance().browseFolder,
                                                           tr("Images (*.tif *.tiff *.png *.jpg *.jpeg *.bmp)"));
            if (!f.isEmpty())
                openInto(i, f);
        });
    }
    root->addWidget(split, 1);
    connect(m_view[0], &ImageView::viewChanged, this, [this] { sync(m_view[0], m_view[1]); });
    connect(m_view[1], &ImageView::viewChanged, this, [this] { sync(m_view[1], m_view[0]); });
}

bool CompareWindow::openLeft(const QString &path) { return openInto(0, path); }
bool CompareWindow::openRight(const QString &path) { return openInto(1, path); }

bool CompareWindow::openInto(int side, const QString &path)
{
    LoadedImage li;
    QString err;
    if (!loadImage(path, li, &err)) {
        QMessageBox::warning(this, tr("Compare"), tr("Cannot open %1:\n%2").arg(path, err));
        return false;
    }
    m_view[side]->setImage(toQImage8(li.data), true);
    m_view[side]->setUmPerPixel(li.meta.umPerPixel);
    QString info = QFileInfo(path).fileName();
    if (!li.meta.objective.isEmpty())
        info += QStringLiteral("  ·  ") + li.meta.objective;
    info += QStringLiteral("  ·  %1 × %2").arg(li.data.width).arg(li.data.height);
    m_label[side]->setText(info);
    return true;
}

void CompareWindow::sync(ImageView *from, ImageView *to)
{
    if (!m_sync->isChecked() || m_syncing || to->image().isNull() || from->image().isNull())
        return;
    m_syncing = true;
    // same relative position; zoom scaled so both show the same field when the
    // images have different pixel counts (e.g. pixel shift vs standard)
    const double scale = double(from->image().width()) / to->image().width();
    to->setViewState(from->relativeCenter(), from->zoom() * scale, from->isFit());
    m_syncing = false;
}

} // namespace lm
