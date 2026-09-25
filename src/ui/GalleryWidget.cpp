#include "GalleryWidget.h"

#include "io/ImageIO.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

namespace lm {

QImage makeThumbnail(const QImage &src, int size)
{
    if (src.isNull())
        return {};
    QImage t = src.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return t.convertToFormat(QImage::Format_RGB888);
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

GalleryWidget::GalleryWidget(QWidget *parent) : QListWidget(parent)
{
    setObjectName(QStringLiteral("Gallery"));
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setWrapping(false);
    setIconSize(QSize(150, 100));
    setGridSize(QSize(170, 132));
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setTextElideMode(Qt::ElideMiddle);
    setMinimumHeight(140);
    connect(this, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        emit openRequested(it->data(Qt::UserRole).toString());
    });
}

void GalleryWidget::addImage(const QString &path, const QImage &src)
{
    auto *it = new QListWidgetItem(thumbnailIcon(makeThumbnail(src)), QFileInfo(path).fileName());
    it->setData(Qt::UserRole, path);
    it->setToolTip(path);
    insertItem(0, it);
    setCurrentItem(it);
    scrollToItem(it);
}

void GalleryWidget::addFile(const QString &path)
{
    auto *it = new QListWidgetItem(QFileInfo(path).fileName());
    it->setData(Qt::UserRole, path);
    it->setToolTip(path);
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
    auto *open = m.addAction(tr("Open in Process"));
    auto *ext = m.addAction(tr("Open with default application"));
    auto *reveal = m.addAction(tr("Show in Explorer"));
    auto *copy = m.addAction(tr("Copy path"));
    m.addSeparator();
    auto *del = m.addAction(tr("Delete file…"));
    QAction *a = m.exec(e->globalPos());
    if (a == open)
        emit openRequested(path);
    else if (a == ext)
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    else if (a == reveal)
        QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
    else if (a == copy)
        QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
    else if (a == del) {
        if (QMessageBox::question(this, tr("Delete"), tr("Move %1 to the recycle bin?").arg(QFileInfo(path).fileName()))
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
