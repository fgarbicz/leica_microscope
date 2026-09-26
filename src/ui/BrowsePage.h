#pragma once
// Browse workspace: folder tree, thumbnails, preview and metadata.

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

signals:
    void openInProcess(const QString &path);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    QToolBar *m_toolbar = nullptr;
    void showPreview(const QString &path);
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
};

} // namespace lm
