#pragma once
// The images captured in this session, in one of two layouts:
//
//   Reel  a horizontal strip of thumbnails under the live image. Costs little
//         height, which matters on a wide screen.
//   List  a vertical column beside the live image, thumbnail and file name per
//         row. Shows far more images at once on a tall screen, and the names
//         are readable, which the reel cannot manage.

#include <QKeySequence>
#include <QListWidget>

namespace lm {

class GalleryWidget : public QListWidget {
    Q_OBJECT
public:
    explicit GalleryWidget(QWidget *parent = nullptr);
    void addImage(const QString &path, const QImage &thumbnailSource);
    void addFile(const QString &path); // loads the thumbnail asynchronously
    // A project: every image in `folder` (oldest first) that is not listed yet;
    // returns how many were added.
    int addFolder(const QString &folder);
    QStringList paths() const;

    // Vertical list beside the image, or horizontal reel below it.
    void setVertical(bool on);
    bool isVertical() const { return m_vertical; }
    // In the list: a small thumbnail and the name on one line, to fit many images.
    void setCompact(bool on);
    bool isCompact() const { return m_compact; }

signals:
    void openRequested(const QString &path);
    void revealRequested(const QString &path);
    void referenceRequested(const QString &path); // blend over the live image for alignment
    void renamed(const QString &from, const QString &to); // file (and sidecars) renamed on disk
    void folderRenamed(const QString &from, const QString &to); // a project folder renamed on disk

protected:
    void contextMenuEvent(QContextMenuEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    // the thumbnail sizes are in scaled pixels, so they are redone when the
    // interface size changes
    void changeEvent(QEvent *e) override;
    // says what the empty strip is for, instead of showing a blank panel
    void paintEvent(QPaintEvent *e) override;

private:
    void applyLayout();
    void renameItem(QListWidgetItem *it); // asks for the new name (F2 / context menu)

    // Project folders: in the list layouts the images are grouped under a
    // heading row per folder they were saved in (the "project"); saving in
    // another folder starts a new group. The heading is hidden in the reel.
    static bool isHeader(const QListWidgetItem *it);
    QListWidgetItem *headerFor(const QString &folder) const;
    QListWidgetItem *addHeader(const QString &folder, int row);
    void placeInGroup(QListWidgetItem *it); // at the end of its folder's group (chronological)
    void removeEmptyHeaders();
    void updateHeaderVisibility();
    void renameFolder(QListWidgetItem *header); // renames the folder on disk
    // the icon size thumbnails are made for (the larger of the two layouts)
    QSize thumbnailSize() const;

    bool m_vertical = false;
    bool m_compact = false;
};

// A thumbnail for an icon of `logical` size (device-independent pixels, i.e.
// already through px()) on a screen with `devicePixelRatio`: made at the
// number of physical pixels the icon covers, so it stays sharp on a high-DPI
// screen and at a large interface size. Safe to call from a worker thread.
QImage makeThumbnail(const QImage &src, QSize logical, qreal devicePixelRatio);
// View -> Reference overlay; shown in the thumbnail menu as the platform spells it
QKeySequence referenceOverlayShortcut();
// Icon that keeps its true colours when the item is selected.
QIcon thumbnailIcon(const QImage &thumb);

} // namespace lm
