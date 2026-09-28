#include "BrowsePage.h"

#include "ui/Icons.h"
#include "ui/PlatformUi.h"
#include "ui/Theme.h"

#include "app/AppSettings.h"
#include "io/ImageIO.h"
#include "ui/BatchExportDialog.h"
#include "ui/BatchIhcDialog.h"
#include "ui/CompareWindow.h"
#include "ui/GalleryWidget.h"
#include "ui/ImageView.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QAbstractFileIconProvider>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSplitter>
#include <QStyle>
#include <QTableWidget>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace lm {

namespace {
const QStringList kImageFilters = {QStringLiteral("*.lif"), // a Leica session, opened image by image
                                  QStringLiteral("*.tif"), QStringLiteral("*.tiff"), QStringLiteral("*.png"),
                                   QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.bmp")};

// Icons and type names for the folder tree from the name alone. The default
// provider asks the system for every entry's own icon (NSWorkspace on macOS, the
// shell on Windows), and names its type by reading the start of the file
// (QMimeDatabase): on the interface thread, whenever the folder changes. In a
// OneDrive or iCloud folder that read downloads the file first - a 442 MB .lif
// froze Browse for as long as that took. The tree shows neither anyway.
class TypeIconProvider : public QAbstractFileIconProvider {
public:
    QString type(const QFileInfo &info) const override
    {
        return info.isDir() ? QStringLiteral("Folder") : info.suffix();
    }
    QIcon icon(IconType type) const override
    {
        QStyle *st = QApplication::style();
        switch (type) {
        case Computer: return st->standardIcon(QStyle::SP_ComputerIcon);
        case Drive: return st->standardIcon(QStyle::SP_DriveHDIcon);
        case Network: return st->standardIcon(QStyle::SP_DriveNetIcon);
        case File: return st->standardIcon(QStyle::SP_FileIcon);
        default: return st->standardIcon(QStyle::SP_DirIcon);
        }
    }
    QIcon icon(const QFileInfo &info) const override
    {
        if (info.isRoot())
            return icon(Drive);
        return icon(info.isDir() ? Folder : File);
    }
};

// The files that belong to an image and travel with it.

// Why `name` cannot be a file name, or an empty string when it can.
} // namespace

BrowsePage::BrowsePage(QWidget *parent) : QWidget(parent)
{
    m_thumbPool.setMaxThreadCount(2);
    m_listPool.setMaxThreadCount(1);
    m_previewPool.setMaxThreadCount(1);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *tb = new QToolBar(this);
    // icon plus label: the icons help people find an action again, the words
    // say what it does the first time
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // allowed to be narrower than its contents: QToolBar then shows its
    // overflow button rather than letting the last actions fall off the edge
    // (which is what happened at a large interface size)
    tb->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_toolbar = tb;
    auto *up = tb->addAction(icon(Icon::Up, theme().subText, 16), tr("Up"));
    auto *refreshAct = tb->addAction(icon(Icon::Refresh, theme().subText, 16), tr("Refresh"));
    tb->addSeparator();
    auto *openProc = tb->addAction(icon(Icon::Process, theme().subText, 16), tr("Open in Process"));
    auto *openExt = tb->addAction(icon(Icon::Open, theme().subText, 16), tr("Open in image viewer"));
    auto *reveal = tb->addAction(icon(Icon::Folder, theme().subText, 16), revealActionText());
    tb->addSeparator();
    auto *exportAct = tb->addAction(icon(Icon::Export, theme().subText, 16), tr("Export…"));
    exportAct->setToolTip(tr("Export the selected images (JPEG/PNG, scale bar, annotations, resize)"));
    auto *ihc = tb->addAction(icon(Icon::Ihc, theme().subText, 16), tr("IHC quantification…"));
    ihc->setToolTip(tr("DAB quantification of the selected images (or all images in the folder), exported as CSV"));
    auto *compare = tb->addAction(icon(Icon::Compare, theme().subText, 16), tr("Compare…"));
    compare->setToolTip(tr("Compare two selected images side by side"));
    auto *rename = tb->addAction(tr("Rename…"));
    auto *del = tb->addAction(icon(Icon::Trash, theme().danger, 16), tr("Delete…"));
    root->addWidget(tb);

    m_header = new QLabel(this);
    m_header->setContentsMargins(px(8), px(4), px(8), px(4));
    root->addWidget(m_header);

    auto *split = new QSplitter(Qt::Horizontal, this);
    // the model is created in ensureLoaded(); see the header
    m_tree = new QTreeView(split);
    m_tree->setHeaderHidden(true);
    m_tree->setMinimumWidth(px(200));

    m_grid = new QListWidget(split);
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setIconSize(QSize(px(160), px(110)));
    m_grid->setGridSize(QSize(px(180), px(150)));
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setTextElideMode(Qt::ElideMiddle);
    m_grid->setWordWrap(true);

    auto *right = new QSplitter(Qt::Vertical, split);
    m_preview = new ImageView(right);
    m_preview->setPlaceholder(tr("Select an image"));
    m_meta = new QTableWidget(0, 2, right);
    m_meta->horizontalHeader()->setVisible(false);
    m_meta->verticalHeader()->setVisible(false);
    m_meta->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_meta->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_meta->setSelectionMode(QAbstractItemView::NoSelection);
    right->setStretchFactor(0, 3);
    right->setStretchFactor(1, 2);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    split->setStretchFactor(2, 3);
    split->setSizes({px(220), px(600), px(520)});
    root->addWidget(split, 1);

    // (the tree's selection model only exists once ensureLoaded() sets a model)
    connect(m_grid, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
        if (it)
            showPreview(it->data(Qt::UserRole).toString());
    });
    connect(m_grid, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        emit openInProcess(it->data(Qt::UserRole).toString());
    });
    connect(up, &QAction::triggered, this, [this] {
        QDir d(m_folder);
        if (d.cdUp())
            setFolder(d.absolutePath());
    });
    connect(refreshAct, &QAction::triggered, this, &BrowsePage::refresh);
    connect(openProc, &QAction::triggered, this, [this] {
        const auto sel = selectedPaths();
        if (!sel.isEmpty())
            emit openInProcess(sel.first());
    });
    connect(openExt, &QAction::triggered, this, [this] {
        for (const auto &p : selectedPaths())
            if (!openInImageViewer(p, this))
                break;
    });
    connect(reveal, &QAction::triggered, this, [this] {
        const auto sel = selectedPaths();
        if (!sel.isEmpty())
            revealInFileManager(sel.first());
        else
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_folder));
    });
    connect(exportAct, &QAction::triggered, this, [this] {
        QStringList sel = selectedPaths();
        if (sel.isEmpty())
            for (int i = 0; i < m_grid->count(); ++i)
                sel << m_grid->item(i)->data(Qt::UserRole).toString();
        if (sel.isEmpty())
            return;
        BatchExportDialog dlg(sel, this);
        dlg.exec();
    });
    connect(ihc, &QAction::triggered, this, [this] {
        QStringList sel = selectedPaths();
        if (sel.isEmpty())
            for (int i = 0; i < m_grid->count(); ++i)
                sel << m_grid->item(i)->data(Qt::UserRole).toString();
        if (sel.isEmpty())
            return;
        BatchIhcDialog dlg(sel, this);
        dlg.exec();
    });
    connect(compare, &QAction::triggered, this, [this] {
        const auto sel = selectedPaths();
        auto *w = new CompareWindow(this);
        if (sel.size() >= 1)
            w->openLeft(sel[0]);
        if (sel.size() >= 2)
            w->openRight(sel[1]);
        w->show();
    });
    connect(rename, &QAction::triggered, this, [this] {
        const auto sel = selectedPaths();
        if (sel.size() != 1)
            return;
        const QFileInfo fi(sel.first());
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename"), tr("New name:"), QLineEdit::Normal,
                                                   fi.completeBaseName(), &ok);
        if (!ok || name == fi.completeBaseName())
            return;
        if (const QString why = invalidFileName(name); !why.isEmpty()) {
            QMessageBox::warning(this, tr("Rename"), why);
            return;
        }
        const QString np = fi.dir().filePath(name + QLatin1Char('.') + fi.suffix());
        QString err;
        if (!renameImage(fi.filePath(), np, &err))
            QMessageBox::warning(this, tr("Rename"), err);
        refresh();
    });
    connect(del, &QAction::triggered, this, [this] {
        const auto sel = selectedPaths();
        if (sel.isEmpty())
            return;
        if (QMessageBox::question(this, tr("Delete"), tr("Move %n image(s) to the %1?", nullptr, int(sel.size())).arg(trashName()))
            != QMessageBox::Yes)
            return;
        QStringList failed;
        for (const auto &p : sel)
            if (!moveImageToTrash(p))
                failed << QFileInfo(p).fileName();
        refresh();
        if (!failed.isEmpty())
            QMessageBox::warning(this, tr("Delete"),
                                 tr("These could not be moved to the %1 (in use, or no permission?):\n\n%2")
                                     .arg(trashName(), failed.join(QLatin1Char('\n'))));
    });
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_grid, &QListWidget::customContextMenuRequested, this,
            [this, openProc, openExt, reveal, rename, del](const QPoint &pt) {
        if (!m_grid->itemAt(pt))
            return;
        QMenu m(this);
        m.addAction(openProc);
        m.addAction(openExt);
        m.addAction(reveal);
        m.addSeparator();
        auto *copy = m.addAction(tr("Copy path"));
        m.addAction(rename);
        m.addAction(del);
        if (m.exec(m_grid->viewport()->mapToGlobal(pt)) == copy)
            QApplication::clipboard()->setText(QDir::toNativeSeparators(selectedPaths().value(0)));
    });

    // Which folder to open is decided in ensureLoaded(): even asking whether
    // ~/Pictures/DM Imaging exists makes macOS put up a permission dialog, and
    // that must wait until the user actually opens Browse.
}

void BrowsePage::ensureLoaded()
{
    if (m_loaded)
        return;
    m_loaded = true;
    if (m_startFolder.isEmpty()) {
        QString start = AppSettings::instance().browseFolder;
        if (start.isEmpty() || !QDir(start).exists())
            start = AppSettings::instance().capture.folder;
        // the image folder is created by the first capture, not here
        if (!QDir(start).exists())
            start = QDir::homePath();
        m_startFolder = start;
    }
    m_dirs = new QFileSystemModel(this);
    static TypeIconProvider icons; // outlives every model that uses it
    m_dirs->setIconProvider(&icons);
    m_dirs->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
    m_dirs->setRootPath(QString());
    m_tree->setModel(m_dirs);
    for (int c = 1; c < m_dirs->columnCount(); ++c)
        m_tree->hideColumn(c);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &i) {
        setFolder(m_dirs->filePath(i));
    });
    setFolder(m_startFolder);
}

QStringList BrowsePage::selectedPaths() const
{
    QStringList l;
    for (auto *it : m_grid->selectedItems())
        l << it->data(Qt::UserRole).toString();
    return l;
}

void BrowsePage::setFolder(const QString &path)
{
    if (path.isEmpty() || !QDir(path).exists())
        return;
    m_folder = QDir(path).absolutePath();
    AppSettings::instance().browseFolder = m_folder;
    if (!m_dirs) { // not shown yet: remember it for ensureLoaded()
        m_startFolder = m_folder;
        return;
    }
    const QModelIndex idx = m_dirs->index(m_folder);
    if (idx.isValid() && m_tree->currentIndex() != idx) {
        QSignalBlocker b(m_tree->selectionModel());
        m_tree->setCurrentIndex(idx);
        m_tree->scrollTo(idx);
        m_tree->expand(idx);
    }
    refresh();
}

void BrowsePage::refresh()
{
    ensureLoaded();
    const int gen = ++(*m_generation);
    m_thumbPool.clear(); // thumbnails of the previous folder that have not started yet
    m_listPool.clear();
    // keep the image the user was looking at (switching back to Browse refreshes)
    const QString keep = m_grid->currentItem() ? m_grid->currentItem()->data(Qt::UserRole).toString() : QString();
    m_grid->clear();
    m_header->setText(tr("%1 — reading the folder…").arg(QDir::toNativeSeparators(m_folder)));
    const QString folder = m_folder;
    auto generation = m_generation;
    // listing sorted by time stats every file: in the background
    QtConcurrent::run(&m_listPool, [folder, gen, generation] {
        if (generation->load() != gen)
            return QFileInfoList();
        return QDir(folder).entryInfoList(kImageFilters, QDir::Files, QDir::Time);
    }).then(this, [this, keep, gen](const QFileInfoList &files) {
        if (m_generation->load() == gen)
            populate(files, keep, gen);
    });
}

void BrowsePage::populate(const QFileInfoList &files, const QString &keepSelected, int gen)
{
    m_header->setText(tr("%1 — %n image(s)", nullptr, int(files.size())).arg(QDir::toNativeSeparators(m_folder)));
    QPointer<QListWidget> grid = m_grid;
    // correctly sized placeholder so the layout does not change when thumbnails arrive
    QPixmap ph(m_grid->iconSize());
    ph.fill(palette().color(QPalette::AlternateBase));
    const QIcon placeholder(ph);
    auto generation = m_generation;
    // made at the pixels the icon really covers (interface size and screen)
    const QSize thumbSize = m_grid->iconSize();
    const qreal dpr = devicePixelRatioF();
    int keepRow = -1;
    for (const QFileInfo &fi : files) {
        auto *it = new QListWidgetItem(placeholder, fi.fileName());
        it->setData(Qt::UserRole, fi.absoluteFilePath());
        it->setToolTip(QStringLiteral("%1\n%2 KB, %3").arg(fi.fileName()).arg(fi.size() / 1024).arg(
            QLocale().toString(fi.lastModified(), QLocale::ShortFormat)));
        m_grid->addItem(it);
        const QString path = fi.absoluteFilePath();
        if (path == keepSelected)
            keepRow = m_grid->count() - 1;
        // thumbnails are generated in the background; stale results are dropped
        QtConcurrent::run(&m_thumbPool, [path, gen, generation, thumbSize, dpr] {
            if (generation->load() != gen)
                return QImage();
            QImageReader r(path);
            QImage img;
            if (r.canRead()) {
                const QSize s = r.size();
                // decode at reduced size, but never below what the thumbnail needs
                const int need = int(std::max(thumbSize.width(), thumbSize.height()) * dpr);
                const int decode = std::max(480, need);
                if (s.isValid() && std::max(s.width(), s.height()) > decode)
                    r.setScaledSize(s.scaled(decode, decode, Qt::KeepAspectRatio));
                img = r.read();
            }
            if (img.isNull()) {
                LoadedImage li;
                if (loadImage(path, li))
                    img = toQImage8(li.data);
            }
            return makeThumbnail(img, thumbSize, dpr);
        }).then(this, [grid, path, gen, generation](const QImage &thumb) {
            if (!grid || generation->load() != gen || thumb.isNull())
                return;
            for (int i = 0; i < grid->count(); ++i)
                if (grid->item(i)->data(Qt::UserRole).toString() == path) {
                    grid->item(i)->setIcon(thumbnailIcon(thumb));
                    // the item's layout can shift slightly; repaint everything so no stale label remains
                    grid->viewport()->update();
                    break;
                }
        });
    }
    if (m_grid->count() > 0) {
        m_grid->setCurrentRow(keepRow >= 0 ? keepRow : 0);
    } else {
        ++(*m_previewGeneration);
        m_previewPath.clear();
        m_preview->clear();
        m_preview->setPlaceholder(QString());
        m_meta->setRowCount(0);
    }
}

void BrowsePage::showPreview(const QString &path)
{
    // the same, unchanged file is already shown (a refresh reselects it)
    const QDateTime stamp = QFileInfo(path).lastModified();
    if (path == m_previewPath && stamp == m_previewStamp && stamp.isValid())
        return;
    m_previewPath = path;
    m_previewStamp = stamp;
    const int gen = ++(*m_previewGeneration);
    m_previewPool.clear(); // a preview asked for earlier and not started yet
    m_preview->clear();
    m_preview->setPlaceholder(tr("Loading %1…").arg(QFileInfo(path).fileName()));
    m_meta->setRowCount(0);

    struct Preview {
        bool ok = false;
        QString error;
        QImage image;
        double umPerPixel = 0;
        QList<QPair<QString, QString>> rows;
    };
    auto generation = m_previewGeneration;
    QtConcurrent::run(&m_previewPool, [path, gen, generation] {
        Preview pv;
        if (generation->load() != gen)
            return pv;
        LoadedImage li;
        if (!loadImage(path, li, &pv.error))
            return pv;
        pv.ok = true;
        pv.image = toQImage8(li.data);
        pv.umPerPixel = li.meta.umPerPixel;
        const QFileInfo fi(path);
        pv.rows.append({tr("File"), fi.fileName()});
        pv.rows.append({tr("Size on disk"), QStringLiteral("%1 MB").arg(fi.size() / 1048576.0, 0, 'f', 2)});
        if (li.hasMeta) {
            pv.rows.append(li.meta.describe());
        } else {
            pv.rows.append({tr("Image size"),
                            QStringLiteral("%1 × %2 px, %3-bit").arg(li.data.width).arg(li.data.height).arg(li.sourceBitDepth)});
            if (li.meta.umPerPixel > 0)
                pv.rows.append({tr("Pixel size"), QStringLiteral("%1 µm/px").arg(li.meta.umPerPixel, 0, 'g', 5)});
        }
        return pv;
    }).then(this, [this, gen](const Preview &pv) {
        if (m_previewGeneration->load() != gen)
            return; // another image was selected meanwhile
        if (!pv.ok) {
            m_previewPath.clear(); // try again next time
            m_preview->clear();
            m_preview->setPlaceholder(tr("Cannot open: %1").arg(pv.error));
            return;
        }
        m_preview->setImage(pv.image, true);
        m_preview->setUmPerPixel(pv.umPerPixel);
        m_meta->setRowCount(int(pv.rows.size()));
        for (int i = 0; i < pv.rows.size(); ++i) {
            auto *k = new QTableWidgetItem(pv.rows[i].first);
            k->setForeground(palette().color(QPalette::PlaceholderText));
            m_meta->setItem(i, 0, k);
            m_meta->setItem(i, 1, new QTableWidgetItem(pv.rows[i].second));
        }
        m_meta->resizeColumnToContents(0);
    });
}

void BrowsePage::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    fitToolBar(m_toolbar, width());
}

} // namespace lm
