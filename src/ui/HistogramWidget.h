#pragma once
// RGB / luminance histogram with draggable black and white point handles.

#include "imaging/Analysis.h"

#include <QWidget>

namespace lm {

class HistogramWidget : public QWidget {
    Q_OBJECT
public:
    enum class Mode { RGB, Luminance, Overlay };
    explicit HistogramWidget(QWidget *parent = nullptr);

    void setHistogram(const Histogram &h);
    void setMode(Mode m) { m_mode = m; update(); }
    void setLogScale(bool on) { m_log = on; update(); }
    void setLevels(double black, double white); // 0..1
    void setLevelsEditable(bool on) { m_editable = on; update(); }

    QSize sizeHint() const override { return QSize(260, 120); }

signals:
    void levelsChanged(double black, double white);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

private:
    QRectF plotRect() const;
    Histogram m_hist;
    bool m_has = false;
    Mode m_mode = Mode::Overlay;
    bool m_log = false;
    bool m_editable = true;
    double m_black = 0, m_white = 1;
    int m_drag = 0; // 1 black, 2 white
};

} // namespace lm
