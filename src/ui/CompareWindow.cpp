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
    resize(1600, 900);
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
        align();
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
    // about 512 pixels wide is plenty to find a shift and takes milliseconds
    m_grayFactor[side] = std::max(1, int(std::lround(li.data.width / 512.0)));
    m_gray[side] = toGray(li.data, m_grayFactor[side]);
    align();
    sync(m_view[0], m_view[1]);
    return true;
}

void CompareWindow::align()
{
    m_offset = QPointF();
    m_alignInfo->clear();
    if (!m_align->isChecked() || m_gray[0].px.empty() || m_gray[1].px.empty())
        return;
    // phase correlation needs equal sizes: crop both to the common top-left part
    const int w = std::min(m_gray[0].width, m_gray[1].width), h = std::min(m_gray[0].height, m_gray[1].height);
    auto crop = [w, h](const ImageF &g) {
        ImageF c(w, h);
        for (int y = 0; y < h; ++y)
            std::copy_n(&g.px[size_t(y) * g.width], w, &c.px[size_t(y) * w]);
        return c;
    };
    const Shift s = phaseCorrelate(crop(m_gray[0]), crop(m_gray[1]));
    if (s.confidence < 0.03) {
        m_alignInfo->setText(tr("Could not align (little in common)"));
        return;
    }
    // moving(x, y) ~ reference(x + dx, y + dy): a feature at p on the right is at p + d on the left
    m_offset = QPointF(s.dx / m_gray[0].width, s.dy / m_gray[0].height);
    m_alignInfo->setText(tr("Aligned: shifted %1 × %2 px").arg(std::lround(s.dx * m_grayFactor[0])).arg(std::lround(s.dy * m_grayFactor[0])));
}

void CompareWindow::sync(ImageView *from, ImageView *to)
{
    if (!m_sync->isChecked() || m_syncing || to->image().isNull() || from->image().isNull())
        return;
    m_syncing = true;
    // same relative position; zoom scaled so both show the same field when the
    // images have different pixel counts (e.g. pixel shift vs standard)
    const double scale = double(from->image().width()) / to->image().width();
    const QPointF offset = from == m_view[0] ? -m_offset : m_offset;
    // aligned images: follow the tissue even in "fit" (otherwise both just show everything)
    const bool fit = from->isFit() && m_offset.isNull();
    to->setViewState(from->relativeCenter() + offset, from->zoom() * scale, fit);
    m_syncing = false;
}

} // namespace lm
