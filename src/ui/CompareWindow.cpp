#include "ui/Theme.h"
#include "CompareWindow.h"

#include "app/AppSettings.h"
#include "imaging/Registration.h"
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

#include <algorithm>
#include <cmath>

namespace lm {

CompareWindow::CompareWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Compare images"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(px(1600), px(900));
    auto *root = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    m_sync = new QCheckBox(tr("Synchronise zoom && pan"), this);
    m_sync->setChecked(true);
    top->addWidget(m_sync);
    m_align = new QCheckBox(tr("Align images (same area on serial sections)"), this);
    m_align->setChecked(true);
    m_align->setToolTip(tr("Finds how far the tissue is shifted between the two images, so that synchronised "
                           "zoom and pan show the same cells in both. Handles shifts, not rotation."));
    top->addWidget(m_align);
    m_alignInfo = new QLabel(this);
    m_alignInfo->setObjectName(QStringLiteral("Hint"));
    top->addWidget(m_alignInfo);
    top->addStretch();
    connect(m_align, &QCheckBox::toggled, this, [this] {
        showAlignment();
        sync(m_view[0], m_view[1]);
    });
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
                                                           tr("Images (*.tif *.tiff *.png *.jpg *.jpeg *.bmp *.lif)"));
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
    // about 512 pixels wide is plenty to find a shift and takes milliseconds
    // a working copy with the long side near 1024 px (a tall mosaic stays small too);
    // align() puts both on a common 512 px grid
    m_size[side] = QSize(li.data.width, li.data.height);
    m_gray[side] = toGray(li.data, std::max(1, std::max(li.data.width, li.data.height) / 1024));
    align();
    sync(m_view[0], m_view[1]);
    return true;
}

bool CompareWindow::aligned() const
{
    return m_shiftFound && m_align->isChecked();
}

void CompareWindow::showAlignment()
{
    m_alignInfo->setText(m_align->isChecked() ? m_alignText : QString());
}

void CompareWindow::align()
{
    m_shiftFound = false;
    m_alignText.clear();
    if (m_gray[0].px.empty() || m_gray[1].px.empty()) {
        showAlignment();
        return;
    }
    // Both images cover the same field whatever their pixel count (pixel shift vs
    // standard, as in sync()), so put them on one grid: the left image's long side
    // at 512, the right image at the same width.
    const double s = 512.0 / std::max(m_size[0].width(), m_size[0].height());
    m_gridW = std::max(8.0, std::round(m_size[0].width() * s));
    m_gridH[0] = std::max(8.0, std::round(m_size[0].height() * s));
    m_gridH[1] = std::max(8.0, std::round(m_size[1].height() * m_gridW / m_size[1].width()));
    const Shift sh = phaseCorrelate(resample(m_gray[0], int(m_gridW), int(m_gridH[0])),
                                    resample(m_gray[1], int(m_gridW), int(m_gridH[1])));
    if (sh.confidence < 0.03) {
        m_alignText = tr("Could not align (little in common)");
    } else {
        // moving(x, y) ~ reference(x + dx, y + dy): a feature at q on the left is at q - d on the right
        m_shiftFound = true;
        m_shift = QPointF(sh.dx, sh.dy);
        m_alignText = tr("Aligned: shifted %1 × %2 px").arg(std::lround(sh.dx / s)).arg(std::lround(sh.dy / s));
    }
    showAlignment();
}

void CompareWindow::sync(ImageView *from, ImageView *to)
{
    if (!m_sync->isChecked() || m_syncing || to->image().isNull() || from->image().isNull())
        return;
    m_syncing = true;
    // same relative position; zoom scaled so both show the same field when the
    // images have different pixel counts (e.g. pixel shift vs standard)
    const double scale = double(from->image().width()) / to->image().width();
    QPointF rel = from->relativeCenter();
    if (aligned()) {
        // through the common grid: left q -> right q - shift, and back
        const bool fromLeft = from == m_view[0];
        const QPointF q(rel.x() * m_gridW, rel.y() * m_gridH[fromLeft ? 0 : 1]);
        const QPointF p = fromLeft ? q - m_shift : q + m_shift;
        rel = QPointF(p.x() / m_gridW, p.y() / m_gridH[fromLeft ? 1 : 0]);
    }
    // aligned images: follow the tissue even in "fit" (otherwise both just show everything)
    to->setViewState(rel, from->zoom() * scale, from->isFit() && !aligned());
    m_syncing = false;
}

} // namespace lm
