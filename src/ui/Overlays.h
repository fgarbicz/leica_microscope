#pragma once
// Drawing of calibrated overlays (scale bar, grid, crosshair). Shared by the
// live view (screen space) and the exporter (burned into image pixels).

#include "app/AppSettings.h"

#include <QPainter>
#include <QRectF>

namespace lm {

// Chooses a "nice" scale bar length (1, 2, 5 × 10^n µm) about `targetFraction`
// of the image width.
double niceScaleLength(double imageWidthUm, double targetFraction = 0.2);
QString formatLength(double um);
QString formatArea(double um2);

// Draws a scale bar. `imageRect` is where the image is drawn (device coords),
// `pixelsPerUm` in device pixels.
void drawScaleBar(QPainter &p, const QRectF &imageRect, double pixelsPerUm, const OverlaySettings &s,
                  double fontScale = 1.0);
void drawGrid(QPainter &p, const QRectF &imageRect, const OverlaySettings &s);
void drawCrosshair(QPainter &p, const QRectF &imageRect, const OverlaySettings &s);

// Renders the scale bar into an image (for export); returns the modified copy.
QImage burnScaleBar(const QImage &img, double umPerPixel, const OverlaySettings &s);

} // namespace lm
