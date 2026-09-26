#include "ImageView.h"

#include "ui/Theme.h"

#include "ui/Annotations.h"
#include "ui/Overlays.h"

#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace lm {

ImageView::ImageView(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(320, 240);
    m_overlays = &AppSettings::instance().overlays;
}

void ImageView::setReferenceImage(const QImage &img)
{
    m_reference = img;
    update();
}

void ImageView::setReferenceOpacity(double opacity)
{
    m_referenceOpacity = std::clamp(opacity, 0.05, 0.95);
    update();
}

void ImageView::setImage(const QImage &img, bool resetView)
{
    // same image at a different resolution (live preview scaling): keep the view
    if (!resetView && !m_image.isNull() && !img.isNull() && img.size() != m_image.size() && !m_fit) {
        const double fx = double(img.width()) / m_image.width(), fy = double(img.height()) / m_image.height();
        if (std::abs(fx - fy) < 0.01) {
            m_center *= fx;
            m_zoom /= fx;
            m_image = img;
            if (m_layer)
                m_layer->setImageSize(img.size());
            update();
            return;
        }
    }
    const bool sizeChanged = img.size() != m_image.size();
    m_image = img;
    if (m_layer)
        m_layer->setImageSize(img.size());
    if (resetView || sizeChanged || m_fit) {
        if (resetView || sizeChanged)
            m_fit = true;
        updateFit();
    }
    update();
}

void ImageView::clear()
{
    m_image = QImage();
    update();
}

void ImageView::setUmPerPixel(double v)
{
    m_umPerPixel = v;
    if (m_layer)
        m_layer->setUmPerPixel(v);
    update();
}

void ImageView::setAnnotationLayer(AnnotationLayer *layer)
{
    if (m_layer)
        disconnect(m_layer, nullptr, this, nullptr);
    m_layer = layer;
    if (m_layer) {
        connect(m_layer, &AnnotationLayer::changed, this, qOverload<>(&QWidget::update));
        m_layer->setUmPerPixel(m_umPerPixel);
        m_layer->setImageSize(m_image.size());
    }
    update();
}

void ImageView::startPick(PickMode mode, const QString &hint)
{
    m_pick = mode;
    m_pickHint = hint;
    m_pickDragging = false;
    setCursor(Qt::CrossCursor);
    update();
}

void ImageView::cancelPick()
{
    if (m_pick == PickMode::None)
        return;
    m_pick = PickMode::None;
    m_pickDragging = false;
    unsetCursor();
    emit pickCancelled();
    update();
}

QTransform ImageView::imageToWidget() const
{
    QTransform t;
    t.translate(width() / 2.0, height() / 2.0);
    t.scale(m_zoom, m_zoom);
    t.translate(-m_center.x(), -m_center.y());
    return t;
}

QPointF ImageView::widgetToImage(const QPointF &p) const
{
    return (p - QPointF(width() / 2.0, height() / 2.0)) / m_zoom + m_center;
}

QRectF ImageView::imageRect() const
{
    return imageToWidget().mapRect(QRectF(QPointF(0, 0), QSizeF(m_image.size())));
}

void ImageView::updateFit()
{
    if (!m_fit || m_image.isNull())
        return;
    const double z = std::min((width() - 8.0) / m_image.width(), (height() - 8.0) / m_image.height());
    m_zoom = std::max(0.01, z);
    m_center = QPointF(m_image.width() / 2.0, m_image.height() / 2.0);
    emit zoomChanged(m_zoom);
}

void ImageView::clampCenter()
{
    if (m_image.isNull())
        return;
    const double hw = width() / 2.0 / m_zoom, hh = height() / 2.0 / m_zoom;
    const double w = m_image.width(), h = m_image.height();
    m_center.setX(w <= 2 * hw ? w / 2 : std::clamp(m_center.x(), hw, w - hw));
    m_center.setY(h <= 2 * hh ? h / 2 : std::clamp(m_center.y(), hh, h - hh));
}

void ImageView::zoomFit()
{
    m_fit = true;
    updateFit();
    update();
}

void ImageView::zoomActual() { setZoom(1.0); }
void ImageView::zoomIn() { setZoom(m_zoom * 1.25); }
void ImageView::zoomOut() { setZoom(m_zoom / 1.25); }

void ImageView::setZoom(double z, const QPointF &anchor)
{
    if (m_image.isNull())
        return;
    z = std::clamp(z, 0.02, 64.0);
    const QPointF a = anchor.x() < 0 ? QPointF(width() / 2.0, height() / 2.0) : anchor;
    const QPointF imgAtAnchor = widgetToImage(a);
    m_zoom = z;
    m_fit = false;
    m_center = imgAtAnchor - (a - QPointF(width() / 2.0, height() / 2.0)) / m_zoom;
    clampCenter();
    emit zoomChanged(m_zoom);
    update();
}

void ImageView::resizeEvent(QResizeEvent *)
{
    if (m_fit)
        updateFit();
    else
        clampCenter();
}

void ImageView::paintEvent(QPaintEvent *)
{
    static const bool profile = qEnvironmentVariableIsSet("DMI_PROFILE");
    QElapsedTimer timer;
    if (profile)
        timer.start();
    struct Report {
        QElapsedTimer &t; bool on;
        ~Report() {
            static double acc = 0; static int n = 0;
            if (!on) return;
            acc += t.nsecsElapsed() / 1e6;
            if (++n == 50) { qInfo("ImageView paint avg %.2f ms", acc / n); acc = 0; n = 0; }
        }
    } report{timer, profile};
    QPainter p(this);
    // The surround stays dark in both themes: a bright frame around a
    // bright-field image changes how a stain looks to the eye.
    p.fillRect(rect(), theme().canvas);
    if (m_image.isNull()) {
        // readable on that dark surround whichever theme is in use
        p.setPen(QColor(168, 172, 180));
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 1.3);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, m_placeholder.isEmpty() ? tr("No image") : m_placeholder);
        return;
    }
    const QTransform T = imageToWidget();
    const QRectF ir = imageRect();
    // draw only the visible part (fast for large images)
    const QRectF visibleImg = QRectF(widgetToImage(QPointF(0, 0)), widgetToImage(QPointF(width(), height())))
                                  .intersected(QRectF(QPointF(0, 0), QSizeF(m_image.size())));
    if (!visibleImg.isEmpty()) {
        const QRectF src = QRectF(std::floor(visibleImg.left()), std::floor(visibleImg.top()), 0, 0);
        QRectF srcR(src.topLeft(), QPointF(std::ceil(visibleImg.right()), std::ceil(visibleImg.bottom())));
        p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 2.0);
        p.drawImage(T.mapRect(srcR), m_image, srcR);
    }
    if (!m_overlayImage.isNull() && m_overlayImage.size() == m_image.size() && !visibleImg.isEmpty()) {
        QRectF srcR(QPointF(std::floor(visibleImg.left()), std::floor(visibleImg.top())),
                    QPointF(std::ceil(visibleImg.right()), std::ceil(visibleImg.bottom())));
        p.drawImage(T.mapRect(srcR), m_overlayImage, srcR);
    }
    // only over an image of the same shape (a camera frame, not a growing mosaic or
    // a centre ROI, where it could not line up anyway)
    const bool refFits = !m_reference.isNull()
                         && std::abs(double(m_reference.width()) / m_reference.height()
                                     - double(m_image.width()) / m_image.height())
                                < 0.02;
    if (refFits && !visibleImg.isEmpty()) {
        // the visible part of the image, in reference pixels
        const double kx = double(m_reference.width()) / m_image.width(), ky = double(m_reference.height()) / m_image.height();
        p.save();
        p.setOpacity(m_referenceOpacity);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(T.mapRect(visibleImg),
                    m_reference,
                    QRectF(visibleImg.left() * kx, visibleImg.top() * ky, visibleImg.width() * kx, visibleImg.height() * ky));
        p.restore();
    }
    // pixel grid at high zoom
    if (m_zoom >= 16) {
        p.setPen(QColor(0, 0, 0, 60));
        const int x0 = int(std::floor(visibleImg.left())), x1 = int(std::ceil(visibleImg.right()));
        const int y0 = int(std::floor(visibleImg.top())), y1 = int(std::ceil(visibleImg.bottom()));
        for (int x = x0; x <= x1; ++x)
            p.drawLine(T.map(QPointF(x, y0)), T.map(QPointF(x, y1)));
        for (int y = y0; y <= y1; ++y)
            p.drawLine(T.map(QPointF(x0, y)), T.map(QPointF(x1, y)));
    }

    if (m_showOverlays && m_overlays) {
        const QRectF vis = ir.intersected(QRectF(rect()));
        if (m_overlays->grid)
            drawGrid(p, ir, *m_overlays);
        if (m_overlays->crosshair)
            drawCrosshair(p, ir, *m_overlays);
        if (m_overlays->scaleBar && m_umPerPixel > 0)
            drawScaleBar(p, vis, m_zoom / m_umPerPixel, *m_overlays);
    }
    // tiles (mosaic)
    if (!m_tiles.isEmpty()) {
        QPen pen(QColor(255, 255, 255, 90), 1, Qt::DashLine);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        for (const auto &t : m_tiles)
            p.drawRect(T.mapRect(t));
    }
    if (!m_highlight.isNull()) {
        QPen pen(m_highlightColor, 2);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(T.mapRect(m_highlight));
    }
    if (!m_focusRegion.isNull()) {
        QPen pen(QColor(80, 220, 120), 1.5, Qt::DashLine);
        pen.setCosmetic(true);
        p.setPen(pen);
        const QRectF fr(m_focusRegion.x() * m_sensorScale, m_focusRegion.y() * m_sensorScale,
                        m_focusRegion.width() * m_sensorScale, m_focusRegion.height() * m_sensorScale);
        p.drawRect(T.mapRect(fr));
    }
    if (m_layer)
        m_layer->paint(p, T, m_zoom);

    // pick rubber band
    if (m_pick == PickMode::Region && m_pickDragging) {
        const QRectF r = QRectF(T.map(m_pickStart), T.map(m_pickEnd)).normalized();
        p.setPen(QPen(QColor(0, 200, 255), 1.5, Qt::DashLine));
        p.setBrush(QColor(0, 200, 255, 40));
        p.drawRect(r);
    }
    if (m_pick != PickMode::None && !m_pickHint.isEmpty()) {
        const QFontMetrics fm(font());
        const QRect r(width() / 2 - fm.horizontalAdvance(m_pickHint) / 2 - 12, 12, fm.horizontalAdvance(m_pickHint) + 24,
                      fm.height() + 12);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 120, 215, 220));
        p.drawRoundedRect(r, 6, 6);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, m_pickHint);
    }
    if (!m_statusText.isEmpty()) {
        const QFontMetrics fm(font());
        const int w = fm.horizontalAdvance(m_statusText) + 20;
        const QRect r(10, height() - fm.height() - 22, w, fm.height() + 12);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(r, 5, 5);
        p.setPen(QColor(255, 210, 90));
        p.drawText(r, Qt::AlignCenter, m_statusText);
    }
    if (m_overlays && m_overlays->focusAssist && m_focus >= 0)
        drawFocusBar(p);
    if (!m_fit && (ir.width() > width() + 2 || ir.height() > height() + 2))
        drawMinimap(p);
}

void ImageView::drawFocusBar(QPainter &p)
{
    const QRect r(width() - 230, 12, 210, 34);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 170));
    p.drawRoundedRect(r, 6, 6);
    const double frac = m_focusPeak > 0 ? std::clamp(m_focus / m_focusPeak, 0.0, 1.0) : 0.0;
    const QRect bar(r.left() + 10, r.top() + 20, r.width() - 20, 7);
    p.setBrush(QColor(60, 60, 60));
    p.drawRect(bar);
    const QColor c = frac > 0.95 ? QColor(80, 220, 120) : frac > 0.8 ? QColor(240, 200, 60) : QColor(230, 90, 60);
    p.setBrush(c);
    p.drawRect(QRect(bar.left(), bar.top(), int(bar.width() * frac), bar.height()));
    p.setPen(Qt::white);
    QFont f = font();
    f.setPixelSize(11);
    p.setFont(f);
    p.drawText(QRect(r.left() + 10, r.top() + 3, r.width() - 20, 16), Qt::AlignLeft | Qt::AlignVCenter,
               tr("Focus %1  (peak %2)").arg(m_focus, 0, 'f', 1).arg(m_focusPeak, 0, 'f', 1));
}

void ImageView::drawMinimap(QPainter &p)
{
    const double maxW = 180, maxH = 130;
    const double s = std::min(maxW / m_image.width(), maxH / m_image.height());
    // top corner opposite to the scale bar (scale bar positions: 0 TL, 1 TR, 2 BL, 3 BR)
    const bool left = m_overlays && m_overlays->scaleBarPosition == 1;
    const double mw = m_image.width() * s, mh = m_image.height() * s;
    const QRectF mr(left ? 14 : width() - mw - 14, 14, mw, mh);
    p.setPen(QColor(255, 255, 255, 160));
    p.setBrush(Qt::NoBrush);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setOpacity(0.85);
    p.drawImage(mr, m_image);
    p.setOpacity(1.0);
    p.drawRect(mr);
    const QRectF vis = QRectF(widgetToImage(QPointF(0, 0)), widgetToImage(QPointF(width(), height())))
                           .intersected(QRectF(QPointF(0, 0), QSizeF(m_image.size())));
    p.setPen(QPen(QColor(0, 200, 255), 1.5));
    p.drawRect(QRectF(mr.left() + vis.left() * s, mr.top() + vis.top() * s, vis.width() * s, vis.height() * s));
}

void ImageView::wheelEvent(QWheelEvent *e)
{
    if (m_image.isNull())
        return;
    const double steps = e->angleDelta().y() / 120.0;
    setZoom(m_zoom * std::pow(1.2, steps), e->position());
    emit viewChanged();
    e->accept();
}

void ImageView::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    const QPointF ip = widgetToImage(e->position());
    if (m_pick != PickMode::None) {
        if (e->button() == Qt::RightButton) {
            cancelPick();
            return;
        }
        if (e->button() == Qt::LeftButton) {
            if (m_pick == PickMode::Point) {
                m_pick = PickMode::None;
                unsetCursor();
                emit pointPicked(ip.toPoint());
                update();
                return;
            }
            m_pickDragging = true;
            m_pickStart = m_pickEnd = ip;
            return;
        }
    }
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier))) {
        m_panning = true;
        m_panStart = e->pos();
        m_panCenterStart = m_center;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (m_layer && m_layer->mousePress(ip, e->button(), e->modifiers(), m_zoom))
        return;
    if (e->button() == Qt::LeftButton) {
        m_panning = true;
        m_panStart = e->pos();
        m_panCenterStart = m_center;
        setCursor(Qt::ClosedHandCursor);
    }
}

void ImageView::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF ip = widgetToImage(e->position());
    m_cursor = ip;
    const bool inside = !m_image.isNull() && ip.x() >= 0 && ip.y() >= 0 && ip.x() < m_image.width() && ip.y() < m_image.height();
    emit cursorMoved(QPoint(int(std::floor(ip.x())), int(std::floor(ip.y()))), inside);
    if (m_pickDragging) {
        m_pickEnd = ip;
        update();
        return;
    }
    if (m_panning) {
        m_fit = false;
        m_center = m_panCenterStart - QPointF(e->pos() - m_panStart) / m_zoom;
        clampCenter();
        update();
        emit viewChanged();
        return;
    }
    if (m_layer)
        m_layer->mouseMove(ip, e->buttons(), m_zoom);
}

void ImageView::mouseReleaseEvent(QMouseEvent *e)
{
    const QPointF ip = widgetToImage(e->position());
    if (m_pickDragging && e->button() == Qt::LeftButton) {
        m_pickDragging = false;
        QRectF r = QRectF(m_pickStart, ip).normalized().intersected(QRectF(QPointF(0, 0), QSizeF(m_image.size())));
        m_pick = PickMode::None;
        unsetCursor();
        update();
        if (r.width() >= 4 && r.height() >= 4)
            emit regionPicked(r.toAlignedRect());
        else
            emit pickCancelled();
        return;
    }
    if (m_panning) {
        m_panning = false;
        unsetCursor();
        return;
    }
    if (m_layer)
        m_layer->mouseRelease(ip, e->button(), m_zoom);
}

void ImageView::mouseDoubleClickEvent(QMouseEvent *e)
{
    const QPointF ip = widgetToImage(e->position());
    if (m_layer && m_layer->mouseDoubleClick(ip, m_zoom))
        return;
    if (e->button() == Qt::LeftButton) {
        if (m_fit)
            setZoom(1.0, e->position());
        else
            zoomFit();
        emit viewChanged();
    }
}

void ImageView::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape && m_pick != PickMode::None) {
        cancelPick();
        return;
    }
    if (m_layer && m_layer->keyPress(e))
        return;
    switch (e->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomIn(); break;
    case Qt::Key_Minus: zoomOut(); break;
    case Qt::Key_0: zoomFit(); break;
    case Qt::Key_1: zoomActual(); break;
    case Qt::Key_2: setZoom(2.0); break;
    default: QWidget::keyPressEvent(e);
    }
}

void ImageView::leaveEvent(QEvent *)
{
    emit cursorMoved(QPoint(-1, -1), false);
}

void ImageView::contextMenuEvent(QContextMenuEvent *e)
{
    if (m_pick != PickMode::None || (m_layer && m_layer->isDrawing()))
        return;
    if (m_layer && m_layer->tool() != 0) // drawing tools use the right button
        return;
    emit contextMenuRequested(e->globalPos());
}

QPointF ImageView::relativeCenter() const
{
    if (m_image.isNull())
        return {0.5, 0.5};
    return {m_center.x() / m_image.width(), m_center.y() / m_image.height()};
}

void ImageView::setViewState(const QPointF &rel, double zoom, bool fit)
{
    if (m_image.isNull())
        return;
    if (fit) {
        zoomFit();
        return;
    }
    m_fit = false;
    m_zoom = std::clamp(zoom, 0.02, 64.0);
    m_center = QPointF(rel.x() * m_image.width(), rel.y() * m_image.height());
    clampCenter();
    emit zoomChanged(m_zoom);
    update();
}

QImage ImageView::renderWithOverlays(bool scaleBar, bool annotations) const
{
    if (m_image.isNull())
        return {};
    QImage out = m_image.convertToFormat(QImage::Format_RGB888);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    if (scaleBar && m_umPerPixel > 0 && m_overlays)
        drawScaleBar(p, QRectF(0, 0, out.width(), out.height()), 1.0 / m_umPerPixel, *m_overlays);
    if (annotations && m_layer)
        m_layer->paint(p, QTransform(), 1.0, true);
    return out;
}

} // namespace lm
