#pragma once
// Thumbnail strip of images captured in this session.

#include <QListWidget>

namespace lm {

class GalleryWidget : public QListWidget {
    Q_OBJECT
public:
    explicit GalleryWidget(QWidget *parent = nullptr);
    void addImage(const QString &path, const QImage &thumbnailSource);
    void addFile(const QString &path); // loads the thumbnail asynchronously
    QStringList paths() const;

signals:
    void openRequested(const QString &path);
    void revealRequested(const QString &path);
    void referenceRequested(const QString &path); // blend over the live image for alignment

protected:
    void contextMenuEvent(QContextMenuEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
};

QImage makeThumbnail(const QImage &src, int size = 160);
// Icon that keeps its true colours when the item is selected.
QIcon thumbnailIcon(const QImage &thumb);

} // namespace lm
