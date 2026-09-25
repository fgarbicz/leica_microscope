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

protected:
    void contextMenuEvent(QContextMenuEvent *e) override;
};

QImage makeThumbnail(const QImage &src, int size = 160);

} // namespace lm
