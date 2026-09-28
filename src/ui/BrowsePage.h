#pragma once
// Browse workspace: folder tree, thumbnails, preview and metadata.

#include <QDateTime>
#include <QFileInfoList>
#include <QThreadPool>
#include <QWidget>

#include <atomic>
#include <memory>

class QTreeView;
class QFileSystemModel;
class QListWidget;
class QTableWidget;
class QLabel;

class QToolBar;

namespace lm {

class ImageView;

class BrowsePage : public QWidget {
    Q_OBJECT
public:
    explicit BrowsePage(QWidget *parent = nullptr);
    void setFolder(const QString &path);
    QString folder() const { return m_folder; }
    void refresh();
    // Builds the folder tree and loads the starting folder. Called the first
    // time the page is shown, not in the constructor: on macOS listing the
    // home directory asks the user for permission to the Pictures folder, and
    // that must not happen while they are still on the Acquire page.
    void ensureLoaded();
    ImageView *imageView() const { return m_preview; }

signals:
    void openInProcess(const QString &path);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    QToolBar *m_toolbar = nullptr;
    void showPreview(const QString &path);
    // fills the grid from a finished folder listing and starts the thumbnails
    void populate(const QFileInfoList &files, const QString &keepSelected, int gen);
    QStringList selectedPaths() const;

    QTreeView *m_tree;
    QFileSystemModel *m_dirs = nullptr;
    QString m_startFolder;
    bool m_loaded = false;
    QListWidget *m_grid;
    ImageView *m_preview;
    QTableWidget *m_meta;
    QLabel *m_header;
    QString m_folder;
    std::shared_ptr<std::atomic<int>> m_generation = std::make_shared<std::atomic<int>>(0);
    // thumbnails get their own small pool so that capture saves on the global
    // pool never wait behind a folder full of thumbnails
    QThreadPool m_thumbPool;
    // Nothing that reads files runs on the interface thread: a folder may be on
    // a network share or in OneDrive/iCloud, where listing it can wait on the
    // sync app and reading an online-only file downloads it first, and a .lif
    // can be hundreds of MB. Listing and preview each run one at a time, and a
    // newer request makes an older result stale (the generation counters).
    QThreadPool m_listPool;
    QThreadPool m_previewPool;
    std::shared_ptr<std::atomic<int>> m_previewGeneration = std::make_shared<std::atomic<int>>(0);
    QString m_previewPath;     // what the preview shows, and the file's time then
    QDateTime m_previewStamp;
};

} // namespace lm
