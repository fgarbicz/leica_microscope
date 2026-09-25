#pragma once
// Browse workspace: folder tree, thumbnails, preview and metadata.

#include <QWidget>

#include <atomic>
#include <memory>

class QTreeView;
class QFileSystemModel;
class QListWidget;
class QTableWidget;
class QLabel;

namespace lm {

class ImageView;

class BrowsePage : public QWidget {
    Q_OBJECT
public:
    explicit BrowsePage(QWidget *parent = nullptr);
    void setFolder(const QString &path);
    QString folder() const { return m_folder; }
    void refresh();

signals:
    void openInProcess(const QString &path);

private:
    void showPreview(const QString &path);
    QStringList selectedPaths() const;

    QTreeView *m_tree;
    QFileSystemModel *m_dirs;
    QListWidget *m_grid;
    ImageView *m_preview;
    QTableWidget *m_meta;
    QLabel *m_header;
    QString m_folder;
    std::shared_ptr<std::atomic<int>> m_generation = std::make_shared<std::atomic<int>>(0);
};

} // namespace lm
