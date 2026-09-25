#pragma once
// Side-by-side comparison of two images with synchronised zoom and pan.

#include <QWidget>

class QCheckBox;
class QLabel;

namespace lm {

class ImageView;

class CompareWindow : public QWidget {
    Q_OBJECT
public:
    explicit CompareWindow(QWidget *parent = nullptr);
    bool openLeft(const QString &path);
    bool openRight(const QString &path);

private:
    bool openInto(int side, const QString &path);
    void sync(ImageView *from, ImageView *to);

    ImageView *m_view[2];
    QLabel *m_label[2];
    QCheckBox *m_sync;
    bool m_syncing = false;
};

} // namespace lm
