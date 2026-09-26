#pragma once
// Side-by-side comparison of two images with synchronised zoom and pan,
// optionally aligned (serial sections: the same tissue shifted between slides).

#include "core/Frame.h"

#include <QPointF>
#include <QSize>
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
    ImageF m_gray[2];       // downscaled grayscale for alignment (long side ~1024)
    QSize m_size[2];        // full image sizes
    // alignment, in a common grid where both images have the same scale: a
    // feature at q on the left is at q - m_shift on the right
    bool m_aligned = false;
    QPointF m_shift;
    double m_gridW = 0, m_gridH[2] = {0, 0};
    bool m_syncing = false;
};

} // namespace lm
