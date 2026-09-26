#include "ui/Theme.h"
#include "GalleryWidget.h"

#include "ui/PlatformUi.h"

#include "io/ImageIO.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QtConcurrent/QtConcurrentRun>

namespace lm {

QImage makeThumbnail(const QImage &src, int size)
{
    if (src.isNull())
        return {};
    QImage t = src.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return t.convertToFormat(QImage::Format_RGB888);
}

QKeySequence referenceOverlayShortcut()
{
    return QKeySequence(Qt::CTRL | Qt::Key_R); // Cmd+R on macOS
}

QIcon thumbnailIcon(const QImage &thumb)
{
    const QPixmap pm = QPixmap::fromImage(thumb);
    QIcon icon;
    icon.addPixmap(pm, QIcon::Normal);
    icon.addPixmap(pm, QIcon::Selected); // no highlight tint over microscopy colours
    icon.addPixmap(pm, QIcon::Active);
    return icon;
}

namespace {
QString itemToolTip(const QString &path)
{
    return QDir::toNativeSeparators(path) + QLatin1Char('\n')
           + GalleryWidget::tr("Double click: open in the image viewer (e.g. as a reference on another screen)\n"
                               "Right click: open in Process, show the file, delete…");
}
} // namespace

GalleryWidget::GalleryWidget(QWidget *parent) : QListWidget(parent)
{
    setObjectName(QStringLiteral("Gallery"));
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setTextElideMode(Qt::ElideMiddle);
    applyLayout();
    // double click opens the image in the system viewer, in its own window (e.g. a
    // reference on a second screen while the next marker is imaged); live keeps running
    connect(this, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *it) { openInImageViewer(it->data(Qt::UserRole).toString(), this); });
}

void GalleryWidget::applyLayout()
{
    if (m_vertical) {
        // one image per row: thumbnail on the left, file name beside it
        setViewMode(QListView::ListMode);
        setFlow(QListView::TopToBottom);
        setWrapping(false);
        setIconSize(QSize(px(112), px(76)));
        setGridSize(QSize()); // let each row size itself around the icon and the name
        setUniformItemSizes(true);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setWordWrap(true);
        setMinimumHeight(0);
        setMinimumWidth(px(210));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    } else {
        setViewMode(QListView::IconMode);
        setFlow(QListView::LeftToRight);
        setWrapping(false);
        setIconSize(QSize(px(150), px(100)));
        setGridSize(QSize(px(170), px(132)));
        setUniformItemSizes(true);
        setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        setWordWrap(false);
        setMinimumWidth(0);
        setMinimumHeight(px(140));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
}

void GalleryWidget::setVertical(bool on)
{
    if (on == m_vertical)
        return;
    m_vertical = on;
    applyLayout();
}

void GalleryWidget::changeEvent(QEvent *e)
{
    QListWidget::changeEvent(e);
    if (e->type() == QEvent::StyleChange || e->type() == QEvent::FontChange)
        applyLayout(); // the sizes above are in scaled pixels
}

void GalleryWidget::paintEvent(QPaintEvent *e)
{
    QListWidget::paintEvent(e);
    if (count() > 0)
        return;
    // An empty column beside the image reads as a fault; say what it is for.
    QPainter p(viewport());
    p.setPen(theme().faintText);
    const QRect r = viewport()->rect().adjusted(px(10), px(10), -px(10), -px(10));
    p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap,
               tr("Images you capture\nin this session\nappear here"));
}

void GalleryWidget::keyPressEvent(QKeyEvent *e)
{
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && currentItem()) {
        openInImageViewer(currentItem()->data(Qt::UserRole).toString(), this);
        return;
    }
    QListWidget::keyPressEvent(e);
}

void GalleryWidget::addImage(const QString &path, const QImage &src)
{
    auto *it = new QListWidgetItem(thumbnailIcon(makeThumbnail(src)), QFileInfo(path).fileName());
    it->setData(Qt::UserRole, path);
    it->setToolTip(itemToolTip(path));
    insertItem(0, it);
    setCurrentItem(it);
    scrollToItem(it);
}

void GalleryWidget::addFile(const QString &path)
{
    auto *it = new QListWidgetItem(QFileInfo(path).fileName());
    it->setData(Qt::UserRole, path);
    it->setToolTip(itemToolTip(path));
    addItem(it);
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, path] {
        const QImage img = watcher->result();
        for (int i = 0; i < count(); ++i)
            if (item(i)->data(Qt::UserRole).toString() == path)
                item(i)->setIcon(thumbnailIcon(img));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path] {
        LoadedImage li;
        if (!loadImage(path, li))
            return QImage();
        return makeThumbnail(toQImage8(li.data));
    }));
}

QStringList GalleryWidget::paths() const
{
    QStringList l;
    for (int i = 0; i < count(); ++i)
        l << item(i)->data(Qt::UserRole).toString();
    return l;
}

void GalleryWidget::contextMenuEvent(QContextMenuEvent *e)
{
    QListWidgetItem *it = itemAt(e->pos());
    if (!it)
        return;
    const QString path = it->data(Qt::UserRole).toString();
    QMenu m(this);
    auto *ext = m.addAction(tr("Open in image viewer"));
    m.setDefaultAction(ext); // what a double click does
    // the shortcut as this platform spells it (Cmd on macOS)
    auto *ref = m.addAction(
        tr("Overlay on live image (%1)").arg(referenceOverlayShortcut().toString(QKeySequence::NativeText)));
    ref->setToolTip(tr("Shows this image semi-transparently over the live image, to find the same area "
                       "on the next section"));
    auto *open = m.addAction(tr("Open in Process"));
    auto *reveal = m.addAction(revealActionText());
    auto *copy = m.addAction(tr("Copy path"));
    m.addSeparator();
    auto *del = m.addAction(tr("Delete file…"));
    QAction *a = m.exec(e->globalPos());
    if (a == open)
        emit openRequested(path);
    else if (a == ext)
        openInImageViewer(path, this);
    else if (a == ref)
        emit referenceRequested(path);
    else if (a == reveal)
        revealInFileManager(path);
    else if (a == copy)
        QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
    else if (a == del) {
        if (QMessageBox::question(this, tr("Delete"), tr("Move %1 to the %2?").arg(QFileInfo(path).fileName(), trashName()))
            == QMessageBox::Yes) {
            if (QFile::moveToTrash(path)) {
                QFile::moveToTrash(path + QStringLiteral(".json"));
                QFile::moveToTrash(path + QStringLiteral(".annotations.json"));
                delete it;
            }
        }
    }
}

} // namespace lm
