#pragma once
// Zoomable, pannable image display with calibrated overlays, region
// selection and an optional annotation layer.

#include "app/AppSettings.h"
#include "ui/Annotations.h"

#include <QImage>
#include <QPointer>
#include <QWidget>

namespace lm {



class ImageView : public QWidget {
    Q_OBJECT
public:
    enum class PickMode { None, Region, Point };

    explicit ImageView(QWidget *parent = nullptr);

    void setImage(const QImage &img, bool resetView = false);
    const QImage &image() const { return m_image; }
    void clear();

    void setUmPerPixel(double v);
    double umPerPixel() const { return m_umPerPixel; }
    void setOverlaySettings(const OverlaySettings *s) { m_overlays = s; update(); }
    void setAnnotationLayer(AnnotationLayer *layer);
    AnnotationLayer *annotationLayer() const { return m_layer; }

    // Interactive picking (white balance region, focus region, calibration line)
    void startPick(PickMode mode, const QString &hint);
    void cancelPick();
    PickMode pickMode() const { return m_pick; }

    // Additional overlays
    // focus region in sensor pixels; drawn scaled by the sensor scale
    void setFocusRegion(const QRectF &r) { m_focusRegion = r; update(); }
    void setSensorScale(double s) { m_sensorScale = s; }
    void setHighlightRect(const QRectF &r, const QColor &c = QColor(0, 200, 255)) { m_highlight = r; m_highlightColor = c; update(); }
    void setTileRects(const QVector<QRectF> &r) { m_tiles = r; update(); }
    void setStatusText(const QString &t) { m_statusText = t; update(); }
    void setFocusValue(double v, double peak) { m_focus = v; m_focusPeak = peak; update(); }
    void setShowOverlays(bool on) { m_showOverlays = on; update(); }
    void setPlaceholder(const QString &t) { m_placeholder = t; update(); }
    // semi-transparent analysis overlay (ARGB32, same size as the image); null = none
    void setOverlayImage(const QImage &img) { m_overlayImage = img; update(); }
    // earlier image blended over the whole field (stretched to the current image),
    // e.g. to find the same area on the next serial section; null = none
    void setReferenceImage(const QImage &img);
    void setReferenceOpacity(double opacity);
    bool hasReferenceImage() const { return !m_reference.isNull(); }
    // whether the reference has the shape of the current image and is drawn (not
    // over a growing mosaic or a centre ROI, where it could not line up)
    bool referenceFits() const;

    double zoom() const { return m_zoom; }
    bool isFit() const { return m_fit; }
    QTransform imageToWidget() const;
    QPointF widgetToImage(const QPointF &p) const;

    // Renders the currently visible image with overlays at image resolution.
    QImage renderWithOverlays(bool scaleBar, bool annotations) const;

    // View state in relative image coordinates (0..1), for synchronising views
    QPointF relativeCenter() const;
    void setViewState(const QPointF &relCenter, double zoom, bool fit);

public slots:
    void zoomFit();
    void zoomActual();
    void zoomIn();
    void zoomOut();
    void setZoom(double z, const QPointF &anchorWidget = QPointF(-1, -1));

signals:
    void cursorMoved(const QPoint &imagePos, bool inside);
    void regionPicked(const QRect &imageRect);
    void pointPicked(const QPoint &imagePos);
    void pickCancelled();
    void zoomChanged(double zoom);
    void contextMenuRequested(const QPoint &globalPos);
    void viewChanged(); // zoom or pan changed by the user

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

private:
    void updateFit();
    QRectF imageRect() const;
    void clampCenter();
    void drawMinimap(QPainter &p);
    void drawFocusBar(QPainter &p);

    QImage m_image;
    double m_umPerPixel = 0.0;
    const OverlaySettings *m_overlays = nullptr;
    QPointer<AnnotationLayer> m_layer;
    double m_zoom = 1.0;
    QPointF m_center;     // image coordinate shown at the widget centre
    bool m_fit = true;
    bool m_panning = false;
    QPoint m_panStart;
    QPointF m_panCenterStart;
    PickMode m_pick = PickMode::None;
    QString m_pickHint;
    bool m_pickDragging = false;
    QPointF m_pickStart, m_pickEnd;
    QRectF m_focusRegion, m_highlight;
    double m_sensorScale = 1.0;
    QColor m_highlightColor;
    QVector<QRectF> m_tiles;
    QString m_statusText, m_placeholder;
    QImage m_overlayImage;
    QImage m_reference; // drawn scaled onto the image, like the image itself
    double m_referenceOpacity = 0.5;
    double m_focus = -1, m_focusPeak = 0;
    bool m_showOverlays = true;
    QPointF m_cursor{-1, -1};
};

} // namespace lm
