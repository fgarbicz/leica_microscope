#pragma once
// Side-by-side comparison of two images with synchronised zoom and pan,
// optionally aligned (serial sections: the same tissue shifted between slides).

#include "core/Frame.h"

#include <QPointF>
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
    void align(); // estimates m_offset from the two images


    ImageView *m_view[2];
    QLabel *m_label[2];
    QCheckBox *m_sync;
    QCheckBox *m_align;
    QLabel *m_alignInfo;
    ImageF m_gray[2];       // downscaled grayscale for alignment
    int m_grayFactor[2] = {1, 1};
    QPointF m_offset;       // right image position of a feature = left position - offset (relative units)
    bool m_syncing = false;
};

} // namespace lm
