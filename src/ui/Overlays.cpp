#include "Overlays.h"

#include <QFontMetricsF>
#include <QPainterPath>

#include <cmath>

namespace lm {

double niceScaleLength(double widthUm, double frac)
{
    const double target = std::max(1e-6, widthUm * frac);
    const double p = std::pow(10.0, std::floor(std::log10(target)));
    const double m = target / p;
    const double nice = m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10;
    return nice * p;
}

QString formatLength(double um)
{
    if (um >= 1000.0)
        return QStringLiteral("%1 mm").arg(um / 1000.0, 0, 'g', 4);
    if (um >= 1.0)
        return QStringLiteral("%1 µm").arg(um, 0, 'g', um >= 100 ? 4 : 3);
    return QStringLiteral("%1 nm").arg(um * 1000.0, 0, 'g', 3);
}

QString formatArea(double um2)
{
    if (um2 >= 1e6)
        return QStringLiteral("%1 mm²").arg(um2 / 1e6, 0, 'g', 4);
    return QStringLiteral("%1 µm²").arg(um2, 0, 'g', um2 >= 1000 ? 5 : 4);
}

void drawScaleBar(QPainter &p, const QRectF &r, double pxPerUm, const OverlaySettings &s, double fontScale)
{
    if (pxPerUm <= 0 || r.width() < 40)
        return;
    const double widthUm = r.width() / pxPerUm;
    const double lenUm = s.scaleBarLengthUm > 0 ? s.scaleBarLengthUm : niceScaleLength(widthUm, 0.18);
    const double lenPx = lenUm * pxPerUm;
    if (lenPx < 8 || lenPx > r.width() * 0.9)
        return;
    const double unit = std::clamp(r.width() / 900.0, 0.6, 4.0) * fontScale;
    const double barH = std::max(3.0, 6.0 * unit);
    const double margin = 18.0 * unit;
    QFont f = p.font();
    f.setPixelSize(int(std::max(9.0, 15.0 * unit)));
    f.setBold(true);
    const QString label = formatLength(lenUm);
    const QFontMetricsF fm(f);
    const double textH = fm.height();
    const double boxW = std::max(lenPx, fm.horizontalAdvance(label)) + 16 * unit;
    const double boxH = barH + textH + 14 * unit;
    double x, y;
    switch (s.scaleBarPosition) {
    case 0: x = r.left() + margin; y = r.top() + margin; break;
    case 1: x = r.right() - margin - boxW; y = r.top() + margin; break;
    case 2: x = r.left() + margin; y = r.bottom() - margin - boxH; break;
    default: x = r.right() - margin - boxW; y = r.bottom() - margin - boxH; break;
    }
    const QRectF box(x, y, boxW, boxH);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (s.scaleBarBackground) {
        const bool lightFg = s.scaleBarColor.lightness() > 128;
        p.setPen(Qt::NoPen);
        p.setBrush(lightFg ? QColor(0, 0, 0, 150) : QColor(255, 255, 255, 170));
        p.drawRoundedRect(box, 4 * unit, 4 * unit);
    }
    const double bx = box.center().x() - lenPx / 2;
    const double by = box.bottom() - 7 * unit - barH;
    p.setPen(Qt::NoPen);
    p.setBrush(s.scaleBarColor);
    p.drawRect(QRectF(bx, by, lenPx, barH));
    p.setFont(f);
    p.setPen(s.scaleBarColor);
    p.drawText(QRectF(box.left(), box.top() + 4 * unit, box.width(), textH), Qt::AlignCenter, label);
    p.restore();
}

void drawGrid(QPainter &p, const QRectF &r, const OverlaySettings &s)
{
    const int n = std::max(2, s.gridDivisions);
    p.save();
    QPen pen(s.gridColor);
    pen.setCosmetic(true);
    pen.setWidthF(1.0);
    p.setPen(pen);
    for (int i = 1; i < n; ++i) {
        const double x = r.left() + r.width() * i / n;
        const double y = r.top() + r.height() * i / n;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.restore();
}

void drawCrosshair(QPainter &p, const QRectF &r, const OverlaySettings &s)
{
    p.save();
    QPen pen(s.gridColor.lighter());
    pen.setCosmetic(true);
    pen.setWidthF(1.2);
    p.setPen(pen);
    const QPointF c = r.center();
    const double len = std::min(r.width(), r.height()) * 0.08;
    p.drawLine(QPointF(c.x() - len, c.y()), QPointF(c.x() + len, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - len), QPointF(c.x(), c.y() + len));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, len * 0.35, len * 0.35);
    p.restore();
}

QImage burnScaleBar(const QImage &img, double umPerPixel, const OverlaySettings &s)
{
    if (umPerPixel <= 0)
        return img;
    QImage out = img.convertToFormat(img.depth() > 32 ? QImage::Format_RGBX64 : QImage::Format_RGB888);
    QPainter p(&out);
    drawScaleBar(p, QRectF(0, 0, out.width(), out.height()), 1.0 / umPerPixel, s, 1.0);
    return out;
}

} // namespace lm
