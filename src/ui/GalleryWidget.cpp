#include "ui/Theme.h"
#include "GalleryWidget.h"

#include "ui/Icons.h"
#include "ui/PlatformUi.h"

#include "io/ImageIO.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QUrl>
#include <QEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace lm {

QImage makeThumbnail(const QImage &src, QSize logical, qreal devicePixelRatio)
{
    if (src.isNull() || logical.isEmpty())
        return {};
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatio);
    // never enlarged: a small image stays at its own size
    const QSize target = (QSizeF(logical) * dpr).toSize().boundedTo(src.size());
    QImage t = src.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
    t.setDevicePixelRatio(dpr);
    return t;
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
// Qt::UserRole holds the image path (for a heading: the folder path)
constexpr int kHeaderRole = Qt::UserRole + 1;

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
    connect(this, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        if (isHeader(it))
            renameFolder(it);
        else
            openInImageViewer(it->data(Qt::UserRole).toString(), this);
    });
}

void GalleryWidget::applyLayout()
{
    if (m_vertical && m_compact) {
        // many images at once: a small thumbnail and the name on one line
        setViewMode(QListView::ListMode);
        setFlow(QListView::TopToBottom);
        setWrapping(false);
        setIconSize(QSize(px(40), px(27)));
        setGridSize(QSize());
        setUniformItemSizes(false); // project headings are shorter than image rows
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setWordWrap(false);
        setMinimumHeight(0);
        setMinimumWidth(px(160));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    } else if (m_vertical) {
        // one image per row: thumbnail on the left, file name beside it
        setViewMode(QListView::ListMode);
        setFlow(QListView::TopToBottom);
        setWrapping(false);
        setIconSize(QSize(px(112), px(76)));
        setGridSize(QSize()); // let each row size itself around the icon and the name
        setUniformItemSizes(false); // project headings are shorter than image rows
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
        setUniformItemSizes(false); // project headings are shorter than image rows
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
    updateHeaderVisibility();
}

bool GalleryWidget::isHeader(const QListWidgetItem *it)
{
    return it && it->data(kHeaderRole).toBool();
}

QListWidgetItem *GalleryWidget::headerFor(const QString &folder) const
{
    for (int i = 0; i < count(); ++i)
        if (isHeader(item(i)) && QDir::cleanPath(item(i)->data(Qt::UserRole).toString()) == QDir::cleanPath(folder))
            return item(i);
    return nullptr;
}

QListWidgetItem *GalleryWidget::addHeader(const QString &folder, int row)
{
    auto *h = new QListWidgetItem(icon(Icon::Folder, theme().subText, 16), QFileInfo(folder).fileName());
    h->setData(Qt::UserRole, folder);
    h->setData(kHeaderRole, true);
    h->setFlags(Qt::ItemIsEnabled); // a heading, not an image: not selectable
    QFont f = font();
    f.setBold(true);
    h->setFont(f);
    h->setToolTip(tr("Project folder %1\nDouble click or right click to rename it (renames the folder on disk)")
                      .arg(QDir::toNativeSeparators(folder)));
    insertItem(row, h);
    setRowHidden(row, !m_vertical);
    return h;
}

void GalleryWidget::placeInGroup(QListWidgetItem *it)
{
    // chronological: a new project's group goes last, a new image at the end of its group
    const QString folder = QFileInfo(it->data(Qt::UserRole).toString()).absolutePath();
    QListWidgetItem *h = headerFor(folder);
    if (!h)
        h = addHeader(folder, count());
    int r = row(h) + 1;
    while (r < count() && !isHeader(item(r)))
        ++r;
    insertItem(r, it);
}

int GalleryWidget::addFolder(const QString &folder)
{
    const QStringList listed = paths();
    const QFileInfoList files =
        QDir(folder).entryInfoList({QStringLiteral("*.tif"), QStringLiteral("*.tiff"), QStringLiteral("*.png"),
                                    QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.bmp")},
                                   QDir::Files, QDir::Time | QDir::Reversed); // oldest first
    int added = 0;
    for (const QFileInfo &fi : files)
        if (!listed.contains(fi.absoluteFilePath())) {
            addFile(fi.absoluteFilePath());
            ++added;
        }
    if (QListWidgetItem *h = headerFor(folder))
        scrollToItem(h, QAbstractItemView::PositionAtTop);
    return added;
}

void GalleryWidget::removeEmptyHeaders()
{
    for (int i = count() - 1; i >= 0; --i)
        if (isHeader(item(i)) && (i + 1 >= count() || isHeader(item(i + 1))))
            delete takeItem(i);
}

void GalleryWidget::updateHeaderVisibility()
{
    for (int i = 0; i < count(); ++i)
        if (isHeader(item(i)))
            setRowHidden(i, !m_vertical);
}

void GalleryWidget::setCompact(bool on)
{
    if (on == m_compact)
        return;
    m_compact = on;
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
    if (isHeader(currentItem())) {
        if (e->key() == Qt::Key_F2 || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            renameFolder(currentItem());
            return;
        }
    } else if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && currentItem()) {
        openInImageViewer(currentItem()->data(Qt::UserRole).toString(), this);
        return;
    }
    if (e->key() == Qt::Key_F2 && currentItem()) {
        renameItem(currentItem());
        return;
    }
    QListWidget::keyPressEvent(e);
}

QSize GalleryWidget::thumbnailSize() const
{
    // large enough for either layout, so switching between them stays sharp
    return QSize(px(150), px(100)).expandedTo(QSize(px(112), px(76)));
}

void GalleryWidget::addImage(const QString &path, const QImage &src)
{
    auto *it = new QListWidgetItem(thumbnailIcon(makeThumbnail(src, thumbnailSize(), devicePixelRatioF())),
                                   QFileInfo(path).fileName());
    it->setData(Qt::UserRole, path);
    it->setToolTip(itemToolTip(path));
    placeInGroup(it);
    // the new image alone: without the explicit command an extended selection
    // kept every earlier capture selected too
    setCurrentItem(it, QItemSelectionModel::ClearAndSelect);
    scrollToItem(it);
}

void GalleryWidget::addFile(const QString &path)
{
    auto *it = new QListWidgetItem(QFileInfo(path).fileName());
    it->setData(Qt::UserRole, path);
    it->setToolTip(itemToolTip(path));
    placeInGroup(it);
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, path] {
        const QImage img = watcher->result();
        for (int i = 0; i < count(); ++i)
            if (item(i)->data(Qt::UserRole).toString() == path)
                item(i)->setIcon(thumbnailIcon(img));
        watcher->deleteLater();
    });
    const QSize thumb = thumbnailSize();
    const qreal dpr = devicePixelRatioF();
    watcher->setFuture(QtConcurrent::run([path, thumb, dpr] {
        LoadedImage li;
        if (!loadImage(path, li))
            return QImage();
        return makeThumbnail(toQImage8(li.data), thumb, dpr);
    }));
}

QStringList GalleryWidget::paths() const
{
    QStringList l;
    for (int i = 0; i < count(); ++i)
        if (!isHeader(item(i)))
            l << item(i)->data(Qt::UserRole).toString();
    return l;
}

void GalleryWidget::contextMenuEvent(QContextMenuEvent *e)
{
    QListWidgetItem *it = itemAt(e->pos());
    if (!it)
        return;
    const QString path = it->data(Qt::UserRole).toString();
    if (isHeader(it)) {
        QMenu m(this);
        auto *ren = m.addAction(tr("Rename project folder…"));
        m.setDefaultAction(ren);
        auto *show = m.addAction(tr("Open folder"));
        QAction *a = m.exec(e->globalPos());
        if (a == ren)
            renameFolder(it);
        else if (a == show)
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }
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
    auto *rename = m.addAction(tr("Rename…"));
    rename->setShortcut(QKeySequence(Qt::Key_F2)); // shown in the menu; handled in keyPressEvent
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
    else if (a == rename)
        renameItem(it);
    else if (a == del) {
        if (QMessageBox::question(this, tr("Delete"), tr("Move %1 to the %2?").arg(QFileInfo(path).fileName(), trashName()))
            == QMessageBox::Yes) {
            if (moveImageToTrash(path)) {
                delete it;
                removeEmptyHeaders();
            }
            else
                QMessageBox::warning(this, tr("Delete"),
                                     tr("%1 could not be moved to the %2 (in use, or no permission?).")
                                         .arg(QDir::toNativeSeparators(path), trashName()));
        }
    }
}

void GalleryWidget::renameItem(QListWidgetItem *it)
{
    const QString path = it->data(Qt::UserRole).toString();
    const QFileInfo fi(path);
    QInputDialog dlg(this);
    dlg.setWindowTitle(tr("Rename image"));
    dlg.setLabelText(tr("New name for %1 (the file in %2 is renamed too):")
                         .arg(fi.fileName(), QDir::toNativeSeparators(fi.absolutePath())));
    dlg.setTextValue(fi.completeBaseName());
    dlg.resize(px(460), dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.textValue().trimmed();
    if (name == fi.completeBaseName())
        return;
    if (const QString why = invalidFileName(name); !why.isEmpty()) {
        QMessageBox::warning(this, tr("Rename image"), why);
        return;
    }
    const QString to = fi.dir().filePath(fi.suffix().isEmpty() ? name : name + QLatin1Char('.') + fi.suffix());
    QString err;
    if (!renameImage(path, to, &err)) {
        QMessageBox::warning(this, tr("Rename image"), err);
        return;
    }
    it->setData(Qt::UserRole, to);
    it->setText(QFileInfo(to).fileName());
    it->setToolTip(itemToolTip(to));
    emit renamed(path, to);
}

void GalleryWidget::renameFolder(QListWidgetItem *header)
{
    const QString from = QDir::cleanPath(header->data(Qt::UserRole).toString());
    const QFileInfo fi(from);
    QInputDialog dlg(this);
    dlg.setWindowTitle(tr("Rename project folder"));
    dlg.setLabelText(tr("New name for the folder %1 (renamed on disk, with every image in it):")
                         .arg(QDir::toNativeSeparators(from)));
    dlg.setTextValue(fi.fileName());
    dlg.resize(px(480), dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.textValue().trimmed();
    if (name == fi.fileName())
        return;
    if (const QString why = invalidFileName(name); !why.isEmpty()) {
        QMessageBox::warning(this, tr("Rename project folder"), why);
        return;
    }
    const QString to = QDir::cleanPath(fi.dir().filePath(name));
    const bool caseOnly = to.compare(from, Qt::CaseInsensitive) == 0;
    if (QFileInfo::exists(to) && !caseOnly) {
        QMessageBox::warning(this, tr("Rename project folder"), tr("%1 already exists.").arg(QDir::toNativeSeparators(to)));
        return;
    }
    // a change of case only is the same folder on Windows and macOS: via a temporary name
    const bool ok = caseOnly ? (QDir().rename(from, from + QStringLiteral(".renaming"))
                                && (QDir().rename(from + QStringLiteral(".renaming"), to)
                                    || (QDir().rename(from + QStringLiteral(".renaming"), from), false)))
                             : QDir().rename(from, to);
    if (!ok) {
        QMessageBox::warning(this, tr("Rename project folder"),
                             tr("%1 could not be renamed. A file in it may be open in another program (an image "
                                "viewer, Explorer or Finder), or the folder may be synchronising.")
                                 .arg(QDir::toNativeSeparators(from)));
        return;
    }
    // the heading and every image of the group (and of subfolders) follow
    const QString prefix = from + QLatin1Char('/');
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem *it = item(i);
        const QString p = QDir::cleanPath(it->data(Qt::UserRole).toString());
        QString np;
        if (p == from)
            np = to;
        else if (p.startsWith(prefix, Qt::CaseInsensitive))
            np = to + p.mid(from.size());
        else
            continue;
        it->setData(Qt::UserRole, np);
        if (isHeader(it)) {
            it->setText(QFileInfo(np).fileName());
            it->setToolTip(tr("Project folder %1\nDouble click or right click to rename it (renames the folder on disk)")
                               .arg(QDir::toNativeSeparators(np)));
        } else {
            it->setToolTip(itemToolTip(np));
        }
    }
    emit folderRenamed(from, to);
}

} // namespace lm
