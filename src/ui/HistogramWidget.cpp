#include "HistogramWidget.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace lm {

HistogramWidget::HistogramWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(90);
    setMouseTracking(true);
}

void HistogramWidget::setHistogram(const Histogram &h)
{
    m_hist = h;
    m_has = h.count > 0;
    update();
}

void HistogramWidget::setLevels(double black, double white)
{
    m_black = black;
    m_white = white;
    update();
}

QRectF HistogramWidget::plotRect() const
{
    return QRectF(6, 6, width() - 12, height() - 26);
}

void HistogramWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    p.fillRect(rect(), palette().color(QPalette::Base));
    p.setPen(palette().color(QPalette::Mid));
    p.drawRect(r);
    // quarter grid
    p.setPen(QColor(128, 128, 128, 50));
    for (int i = 1; i < 4; ++i)
        p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()), QPointF(r.left() + r.width() * i / 4, r.bottom()));
    if (!m_has)
        return;

    auto drawChannel = [&](int c, QColor col, bool fill) {
        double mx = 1;
        for (int i = 1; i < 255; ++i) // ignore extreme bins for scaling
            mx = std::max(mx, m_log ? std::log1p(double(m_hist.bins[c][i])) : double(m_hist.bins[c][i]));
        QPainterPath path;
        path.moveTo(r.left(), r.bottom());
        for (int i = 0; i < 256; ++i) {
            double v = m_log ? std::log1p(double(m_hist.bins[c][i])) : double(m_hist.bins[c][i]);
            v = std::min(1.0, v / mx);
            path.lineTo(r.left() + r.width() * (i + 0.5) / 256.0, r.bottom() - v * r.height());
        }
        path.lineTo(r.right(), r.bottom());
        path.closeSubpath();
        if (fill) {
            QColor f = col;
            f.setAlpha(m_mode == Mode::Overlay ? 70 : 140);
            p.setPen(Qt::NoPen);
            p.setBrush(f);
            p.drawPath(path);
        }
        p.setPen(QPen(col, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    p.save();
    p.setClipRect(r);
    if (m_mode == Mode::Luminance) {
        drawChannel(3, palette().color(QPalette::Text), true);
    } else {
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        drawChannel(0, QColor(230, 60, 60), true);
        drawChannel(1, QColor(60, 200, 80), true);
        drawChannel(2, QColor(70, 120, 255), true);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    p.restore();

    // clipping indicators
    if (m_hist.clippedHigh > 0.001) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 60, 60));
        p.drawEllipse(QPointF(r.right() - 6, r.top() + 6), 4, 4);
    }
    if (m_hist.clippedLow > 0.001) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(60, 120, 255));
        p.drawEllipse(QPointF(r.left() + 6, r.top() + 6), 4, 4);
    }

    // level handles
    if (m_editable) {
        for (int k = 0; k < 2; ++k) {
            const double v = k == 0 ? m_black : m_white;
            const double x = r.left() + r.width() * v;
            QPolygonF tri({QPointF(x, r.bottom() + 2), QPointF(x - 6, r.bottom() + 12), QPointF(x + 6, r.bottom() + 12)});
            p.setPen(palette().color(QPalette::Text));
            p.setBrush(k == 0 ? QColor(20, 20, 20) : QColor(245, 245, 245));
            p.drawPolygon(tri);
            p.setPen(QPen(QColor(255, 255, 255, 80), 1, Qt::DashLine));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
    }
    // statistics
    p.setPen(palette().color(QPalette::PlaceholderText));
    QFont f = font();
    f.setPixelSize(10);
    p.setFont(f);
    const QString s = tr("mean R %1  G %2  B %3   clip %4%")
                          .arg(m_hist.mean[0], 0, 'f', 0)
                          .arg(m_hist.mean[1], 0, 'f', 0)
                          .arg(m_hist.mean[2], 0, 'f', 0)
                          .arg(m_hist.clippedHigh * 100, 0, 'f', 2);
    p.drawText(QRectF(r.left(), r.bottom() + 6, r.width(), 14), Qt::AlignRight | Qt::AlignVCenter, s);
}

void HistogramWidget::mousePressEvent(QMouseEvent *e)
{
    if (!m_editable)
        return;
    const QRectF r = plotRect();
    const double x = e->position().x();
    const double bx = r.left() + r.width() * m_black, wx = r.left() + r.width() * m_white;
    m_drag = std::abs(x - bx) < std::abs(x - wx) ? 1 : 2;
    mouseMoveEvent(e);
}

void HistogramWidget::mouseMoveEvent(QMouseEvent *e)
{
    const QRectF r = plotRect();
    if (!m_drag) {
        const double x = e->position().x();
        const bool near = std::abs(x - (r.left() + r.width() * m_black)) < 8 || std::abs(x - (r.left() + r.width() * m_white)) < 8;
        setCursor(near && m_editable ? Qt::SizeHorCursor : Qt::ArrowCursor);
        return;
    }
    const double v = std::clamp((e->position().x() - r.left()) / r.width(), 0.0, 1.0);
    if (m_drag == 1)
        m_black = std::min(v, m_white - 0.01);
    else
        m_white = std::max(v, m_black + 0.01);
    update();
    emit levelsChanged(m_black, m_white);
}

void HistogramWidget::mouseReleaseEvent(QMouseEvent *)
{
    m_drag = 0;
}

void HistogramWidget::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu m(this);
    auto *a1 = m.addAction(tr("RGB overlay"));
    auto *a2 = m.addAction(tr("Luminance"));
    m.addSeparator();
    auto *lg = m.addAction(tr("Logarithmic scale"));
    lg->setCheckable(true);
    lg->setChecked(m_log);
    m.addSeparator();
    auto *rs = m.addAction(tr("Reset levels"));
    QAction *a = m.exec(e->globalPos());
    if (a == a1) setMode(Mode::Overlay);
    else if (a == a2) setMode(Mode::Luminance);
    else if (a == lg) setLogScale(lg->isChecked());
    else if (a == rs) {
        setLevels(0, 1);
        emit levelsChanged(0, 1);
    }
}

} // namespace lm
