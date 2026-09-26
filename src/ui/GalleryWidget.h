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
    QStringList paths() const;

    // Vertical list beside the image, or horizontal reel below it.
    void setVertical(bool on);
    bool isVertical() const { return m_vertical; }

signals:
    void openRequested(const QString &path);
    void revealRequested(const QString &path);
    void referenceRequested(const QString &path); // blend over the live image for alignment

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

    bool m_vertical = false;
};

QImage makeThumbnail(const QImage &src, int size = 160);
// View -> Reference overlay; shown in the thumbnail menu as the platform spells it
QKeySequence referenceOverlayShortcut();
// Icon that keeps its true colours when the item is selected.
QIcon thumbnailIcon(const QImage &thumb);

} // namespace lm
