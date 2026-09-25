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
#include <QSplitter>
#include <QTableWidget>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace lm {

namespace {
const QStringList kImageFilters = {QStringLiteral("*.tif"), QStringLiteral("*.tiff"), QStringLiteral("*.png"),
                                   QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.bmp")};
}

BrowsePage::BrowsePage(QWidget *parent) : QWidget(parent)
{
    m_thumbPool.setMaxThreadCount(2);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *tb = new QToolBar(this);
    // icon plus label: the icons help people find an action again, the words
    // say what it does the first time
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *up = tb->addAction(icon(Icon::Up, theme().subText, 16), tr("Up"));
    auto *refreshAct = tb->addAction(icon(Icon::Refresh, theme().subText, 16), tr("Refresh"));
    tb->addSeparator();
    auto *openProc = tb->addAction(icon(Icon::Process, theme().subText, 16), tr("Open in Process"));
    auto *openExt = tb->addAction(icon(Icon::Open, theme().subText, 16), tr("Open externally"));
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
    m_header->setContentsMargins(8, 4, 8, 4);
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
    split->setSizes({220, 600, 520});
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
            QDesktopServices::openUrl(QUrl::fromLocalFile(p));
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
        if (!ok || name.isEmpty())
            return;
        const QString np = fi.dir().filePath(name + QLatin1Char('.') + fi.suffix());
        if (QFile::rename(fi.filePath(), np)) {
            QFile::rename(fi.filePath() + QStringLiteral(".json"), np + QStringLiteral(".json"));
            QFile::rename(fi.filePath() + QStringLiteral(".annotations.json"), np + QStringLiteral(".annotations.json"));
            refresh();
        } else {
            QMessageBox::warning(this, tr("Rename"), tr("Could not rename the file."));
        }
    });
    connect(del, &QAction::triggered, this, [this] {
        const auto sel = selectedPaths();
        if (sel.isEmpty())
            return;
        if (QMessageBox::question(this, tr("Delete"), tr("Move %n image(s) to the %1?", nullptr, int(sel.size())).arg(trashName()))
            != QMessageBox::Yes)
            return;
        for (const auto &p : sel) {
            QFile::moveToTrash(p);
            QFile::moveToTrash(p + QStringLiteral(".json"));
            QFile::moveToTrash(p + QStringLiteral(".annotations.json"));
        }
        refresh();
    });
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_grid, &QListWidget::customContextMenuRequested, this, [=](const QPoint &pt) {
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
    m_grid->clear();
    const QFileInfoList files = QDir(m_folder).entryInfoList(kImageFilters, QDir::Files, QDir::Time);
    m_header->setText(tr("%1 — %n image(s)", nullptr, int(files.size())).arg(QDir::toNativeSeparators(m_folder)));
    QPointer<QListWidget> grid = m_grid;
    // correctly sized placeholder so the layout does not change when thumbnails arrive
    QPixmap ph(m_grid->iconSize());
    ph.fill(palette().color(QPalette::AlternateBase));
    const QIcon placeholder(ph);
    auto generation = m_generation;
    for (const QFileInfo &fi : files) {
        auto *it = new QListWidgetItem(placeholder, fi.fileName());
        it->setData(Qt::UserRole, fi.absoluteFilePath());
        it->setToolTip(QStringLiteral("%1\n%2 KB, %3").arg(fi.fileName()).arg(fi.size() / 1024).arg(
            QLocale().toString(fi.lastModified(), QLocale::ShortFormat)));
        m_grid->addItem(it);
        const QString path = fi.absoluteFilePath();
        // thumbnails are generated in the background; stale results are dropped
        QtConcurrent::run(&m_thumbPool, [path, gen, generation] {
            if (generation->load() != gen)
                return QImage();
            QImageReader r(path);
            QImage img;
            if (r.canRead()) {
                const QSize s = r.size();
                if (s.isValid() && s.width() > 800)
                    r.setScaledSize(s.scaled(480, 480, Qt::KeepAspectRatio));
                img = r.read();
            }
            if (img.isNull()) {
                LoadedImage li;
                if (loadImage(path, li))
                    img = toQImage8(li.data);
            }
            return makeThumbnail(img, 180);
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
    if (m_grid->count() > 0)
        m_grid->setCurrentRow(0);
    else {
        m_preview->clear();
        m_meta->setRowCount(0);
    }
}

void BrowsePage::showPreview(const QString &path)
{
    LoadedImage li;
    QString err;
    if (!loadImage(path, li, &err)) {
        m_preview->clear();
        m_preview->setPlaceholder(tr("Cannot open: %1").arg(err));
        m_meta->setRowCount(0);
        return;
    }
    m_preview->setImage(toQImage8(li.data), true);
    m_preview->setUmPerPixel(li.meta.umPerPixel);
    QList<QPair<QString, QString>> rows;
    const QFileInfo fi(path);
    rows.append({tr("File"), fi.fileName()});
    rows.append({tr("Size on disk"), QStringLiteral("%1 MB").arg(fi.size() / 1048576.0, 0, 'f', 2)});
    if (li.hasMeta) {
        rows.append(li.meta.describe());
    } else {
        rows.append({tr("Image size"), QStringLiteral("%1 × %2 px, %3-bit").arg(li.data.width).arg(li.data.height).arg(li.sourceBitDepth)});
        if (li.meta.umPerPixel > 0)
            rows.append({tr("Pixel size"), QStringLiteral("%1 µm/px").arg(li.meta.umPerPixel, 0, 'g', 5)});
    }
    m_meta->setRowCount(int(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        auto *k = new QTableWidgetItem(rows[i].first);
        k->setForeground(palette().color(QPalette::PlaceholderText));
        m_meta->setItem(i, 0, k);
        m_meta->setItem(i, 1, new QTableWidgetItem(rows[i].second));
    }
    m_meta->resizeColumnToContents(0);
}

} // namespace lm
