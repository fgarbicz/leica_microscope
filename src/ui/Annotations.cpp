#include "Annotations.h"

#include "ui/Overlays.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>

#include <cmath>

namespace lm {

namespace {
constexpr double kPi = 3.14159265358979323846;

double segDist(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = l2 > 0 ? ((p - a).x() * ab.x() + (p - a).y() * ab.y()) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF q = a + t * ab;
    return QLineF(p, q).length();
}

QRectF rectOf(const Annotation &a)
{
    if (a.pts.size() < 2)
        return {};
    return QRectF(a.pts[0], a.pts[1]).normalized();
}

bool isTwoPoint(Annotation::Type t)
{
    return t == Annotation::Line || t == Annotation::Arrow || t == Annotation::Rectangle || t == Annotation::Ellipse;
}

const char *typeName(Annotation::Type t)
{
    switch (t) {
    case Annotation::Line: return "line";
    case Annotation::Arrow: return "arrow";
    case Annotation::Polyline: return "polyline";
    case Annotation::Rectangle: return "rectangle";
    case Annotation::Ellipse: return "ellipse";
    case Annotation::Polygon: return "polygon";
    case Annotation::Angle: return "angle";
    case Annotation::Text: return "text";
    case Annotation::Count: return "count";
    }
    return "line";
}

Annotation::Type typeFromName(const QString &s)
{
    static const QHash<QString, Annotation::Type> m = {
        {"line", Annotation::Line},       {"arrow", Annotation::Arrow},     {"polyline", Annotation::Polyline},
        {"rectangle", Annotation::Rectangle}, {"ellipse", Annotation::Ellipse}, {"polygon", Annotation::Polygon},
        {"angle", Annotation::Angle},     {"text", Annotation::Text},       {"count", Annotation::Count}};
    return m.value(s, Annotation::Line);
}
} // namespace

AnnotationLayer::AnnotationLayer(QObject *parent) : QObject(parent) {}

void AnnotationLayer::setTool(Tool t)
{
    if (m_drawing)
        finishDrawing();
    m_tool = t;
    emit changed();
}

void AnnotationLayer::setColor(const QColor &c)
{
    m_color = c;
    if (Annotation *a = find(m_selected)) {
        pushUndo();
        a->color = c;
        emit changed();
    }
}

Annotation *AnnotationLayer::find(int id)
{
    for (auto &a : m_items)
        if (a.id == id)
            return &a;
    return nullptr;
}

void AnnotationLayer::select(int id)
{
    if (m_selected == id)
        return;
    m_selected = id;
    emit selectionChanged(id);
    emit changed();
}

void AnnotationLayer::pushUndo()
{
    m_undo.push_back(m_items);
    if (m_undo.size() > 100)
        m_undo.pop_front();
    m_redo.clear();
}

bool AnnotationLayer::undo()
{
    if (m_undo.isEmpty())
        return false;
    m_redo.push_back(m_items);
    m_items = m_undo.takeLast();
    m_selected = 0;
    emit changed();
    return true;
}

bool AnnotationLayer::redo()
{
    if (m_redo.isEmpty())
        return false;
    m_undo.push_back(m_items);
    m_items = m_redo.takeLast();
    m_selected = 0;
    emit changed();
    return true;
}

void AnnotationLayer::clear()
{
    if (m_items.isEmpty())
        return;
    pushUndo();
    m_items.clear();
    m_selected = 0;
    m_drawing = false;
    emit changed();
}

void AnnotationLayer::removeSelected()
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].id == m_selected) {
            pushUndo();
            m_items.removeAt(i);
            m_selected = 0;
            emit selectionChanged(0);
            emit changed();
            return;
        }
}

void AnnotationLayer::addText(const QPointF &pos, const QString &text)
{
    if (text.trimmed().isEmpty())
        return;
    pushUndo();
    Annotation a;
    a.id = m_nextId++;
    a.type = Annotation::Text;
    a.pts = {pos};
    a.text = text;
    a.color = m_color;
    a.lineWidth = m_lineWidth;
    m_items.push_back(a);
    select(a.id);
    emit changed();
}

void AnnotationLayer::setAnnotationText(int id, const QString &text)
{
    if (Annotation *a = find(id)) {
        pushUndo();
        a->text = text;
        emit changed();
    }
}

Measurement AnnotationLayer::measure(const Annotation &a) const
{
    Measurement m;
    const double s = m_umPerPixel > 0 ? m_umPerPixel : 1.0;
    const QString unit = m_umPerPixel > 0 ? QString() : QStringLiteral(" px");
    auto len = [&](double px) { return m_umPerPixel > 0 ? formatLength(px * s) : QStringLiteral("%1 px").arg(px, 0, 'f', 1); };
    auto area = [&](double px2) {
        return m_umPerPixel > 0 ? formatArea(px2 * s * s) : QStringLiteral("%1 px²").arg(px2, 0, 'f', 0);
    };
    switch (a.type) {
    case Annotation::Line:
    case Annotation::Arrow:
        if (a.pts.size() >= 2) {
            const double l = QLineF(a.pts[0], a.pts[1]).length();
            m.lengthUm = l * s;
            m.summary = len(l);
        }
        break;
    case Annotation::Polyline: {
        double l = 0;
        for (int i = 1; i < a.pts.size(); ++i)
            l += QLineF(a.pts[i - 1], a.pts[i]).length();
        m.lengthUm = l * s;
        m.summary = len(l);
        break;
    }
    case Annotation::Rectangle: {
        const QRectF r = rectOf(a);
        m.widthUm = r.width() * s;
        m.heightUm = r.height() * s;
        m.areaUm2 = r.width() * r.height() * s * s;
        m.lengthUm = 2 * (r.width() + r.height()) * s;
        m.summary = QStringLiteral("%1 × %2, %3").arg(len(r.width()), len(r.height()), area(r.width() * r.height()));
        break;
    }
    case Annotation::Ellipse: {
        const QRectF r = rectOf(a);
        const double ra = r.width() / 2, rb = r.height() / 2;
        const double ar = kPi * ra * rb;
        const double h = std::pow(ra - rb, 2) / std::max(1e-9, std::pow(ra + rb, 2));
        const double per = kPi * (ra + rb) * (1 + 3 * h / (10 + std::sqrt(4 - 3 * h)));
        m.widthUm = r.width() * s;
        m.heightUm = r.height() * s;
        m.areaUm2 = ar * s * s;
        m.lengthUm = per * s;
        m.summary = QStringLiteral("Ø %1 × %2, %3").arg(len(r.width()), len(r.height()), area(ar));
        break;
    }
    case Annotation::Polygon: {
        double ar = 0, per = 0;
        const int n = int(a.pts.size());
        for (int i = 0; i < n; ++i) {
            const QPointF &p0 = a.pts[i], &p1 = a.pts[(i + 1) % n];
            ar += p0.x() * p1.y() - p1.x() * p0.y();
            per += QLineF(p0, p1).length();
        }
        ar = std::abs(ar) / 2;
        m.areaUm2 = ar * s * s;
        m.lengthUm = per * s;
        m.summary = QStringLiteral("%1, perimeter %2").arg(area(ar), len(per));
        break;
    }
    case Annotation::Angle:
        if (a.pts.size() >= 3) {
            const QLineF l1(a.pts[1], a.pts[0]), l2(a.pts[1], a.pts[2]);
            double ang = l1.angleTo(l2);
            if (ang > 180)
                ang = 360 - ang;
            m.angleDeg = ang;
            m.summary = QStringLiteral("%1°").arg(ang, 0, 'f', 1);
        }
        break;
    case Annotation::Count:
        m.count = int(a.pts.size());
        m.summary = tr("n = %1").arg(m.count);
        break;
    case Annotation::Text:
        m.summary = a.text;
        break;
    }
    (void)unit;
    return m;
}

int AnnotationLayer::hitTest(const QPointF &p, double tol, int *vertex) const
{
    *vertex = -1;
    for (int k = int(m_items.size()) - 1; k >= 0; --k) {
        const Annotation &a = m_items[k];
        // vertices first
        for (int i = 0; i < a.pts.size(); ++i)
            if (QLineF(p, a.pts[i]).length() <= tol * 1.5) {
                *vertex = (a.type == Annotation::Text || a.type == Annotation::Count) ? -1 : i;
                return a.id;
            }
        switch (a.type) {
        case Annotation::Line:
        case Annotation::Arrow:
        case Annotation::Polyline:
        case Annotation::Angle:
            for (int i = 1; i < a.pts.size(); ++i)
                if (segDist(p, a.pts[i - 1], a.pts[i]) <= tol)
                    return a.id;
            break;
        case Annotation::Rectangle: {
            const QRectF r = rectOf(a);
            if (r.adjusted(-tol, -tol, tol, tol).contains(p) && !r.adjusted(tol, tol, -tol, -tol).contains(p))
                return a.id;
            break;
        }
        case Annotation::Ellipse: {
            const QRectF r = rectOf(a);
            if (r.width() > 0 && r.height() > 0) {
                const double dx = (p.x() - r.center().x()) / (r.width() / 2);
                const double dy = (p.y() - r.center().y()) / (r.height() / 2);
                const double d = std::sqrt(dx * dx + dy * dy);
                if (std::abs(d - 1.0) * std::min(r.width(), r.height()) / 2 <= tol)
                    return a.id;
            }
            break;
        }
        case Annotation::Polygon: {
            const int n = int(a.pts.size());
            for (int i = 0; i < n; ++i)
                if (segDist(p, a.pts[i], a.pts[(i + 1) % n]) <= tol)
                    return a.id;
            break;
        }
        case Annotation::Text:
            if (!a.pts.isEmpty()) {
                const double h = 20.0 * std::max(1.0, a.lineWidth / 2) / std::max(0.05, tol / 6);
                QRectF r(a.pts[0], QSizeF(std::max(30.0, a.text.size() * h * 0.6), h));
                if (r.adjusted(-tol, -tol, tol, tol).contains(p))
                    return a.id;
            }
            break;
        case Annotation::Count:
            break;
        }
    }
    return 0;
}

bool AnnotationLayer::mousePress(const QPointF &p, Qt::MouseButton b, Qt::KeyboardModifiers, double scale)
{
    const double tol = 6.0 / std::max(0.01, scale);
    if (m_tool == SelectTool) {
        if (b != Qt::LeftButton)
            return false;
        int vertex = -1;
        const int id = hitTest(p, tol, &vertex);
        select(id);
        if (id) {
            m_dragId = id;
            m_dragVertex = vertex;
            m_dragStart = p;
            m_dragOrig = find(id)->pts;
            m_dragMoved = false;
            return true;
        }
        return false; // allow panning
    }
    if (m_tool == TextTool) {
        if (b == Qt::LeftButton)
            emit textRequested(p);
        return true;
    }
    if (m_tool == CountTool) {
        if (!m_drawing) {
            // continue the selected count annotation, or start a new one
            Annotation *sel = find(m_selected);
            if (sel && sel->type == Annotation::Count) {
                m_current = *sel;
                for (int i = 0; i < m_items.size(); ++i)
                    if (m_items[i].id == sel->id) {
                        pushUndo();
                        m_items.removeAt(i);
                        break;
                    }
            } else {
                m_current = Annotation();
                m_current.id = m_nextId++;
                m_current.type = Annotation::Count;
                m_current.color = m_color;
                m_current.lineWidth = m_lineWidth;
            }
            m_drawing = true;
        }
        if (b == Qt::LeftButton) {
            m_current.pts.push_back(p);
        } else if (b == Qt::RightButton) {
            // remove nearest marker
            int best = -1;
            double bd = tol * 2;
            for (int i = 0; i < m_current.pts.size(); ++i) {
                const double d = QLineF(p, m_current.pts[i]).length();
                if (d < bd) {
                    bd = d;
                    best = i;
                }
            }
            if (best >= 0)
                m_current.pts.removeAt(best);
        }
        emit changed();
        return true;
    }
    // drawing tools
    if (b == Qt::RightButton) {
        if (m_drawing)
            finishDrawing();
        return true;
    }
    if (b != Qt::LeftButton)
        return false;
    const Annotation::Type type = m_tool == LineTool ? Annotation::Line : m_tool == ArrowTool ? Annotation::Arrow
                                  : m_tool == PolylineTool ? Annotation::Polyline : m_tool == RectTool ? Annotation::Rectangle
                                  : m_tool == EllipseTool ? Annotation::Ellipse : m_tool == PolygonTool ? Annotation::Polygon
                                                                                             : Annotation::Angle;
    if (!m_drawing) {
        m_current = Annotation();
        m_current.id = m_nextId++;
        m_current.type = type;
        m_current.color = m_color;
        m_current.lineWidth = m_lineWidth;
        m_current.pts = {p, p};
        m_drawing = true;
    } else if (!isTwoPoint(type)) {
        m_current.pts.back() = p;
        m_current.pts.push_back(p);
        if (type == Annotation::Angle && m_current.pts.size() > 3) {
            m_current.pts.pop_back();
            finishDrawing();
        }
    }
    m_hover = p;
    emit changed();
    return true;
}

bool AnnotationLayer::mouseMove(const QPointF &p, Qt::MouseButtons b, double)
{
    m_hover = p;
    if (m_drawing && m_current.type != Annotation::Count) {
        if (!m_current.pts.isEmpty())
            m_current.pts.back() = p;
        emit changed();
        return true;
    }
    if (m_dragId && (b & Qt::LeftButton)) {
        Annotation *a = find(m_dragId);
        if (!a)
            return false;
        if (!m_dragMoved) {
            // record undo state before the first modification
            QVector<Annotation> snapshot = m_items;
            for (auto &s : snapshot)
                if (s.id == m_dragId)
                    s.pts = m_dragOrig;
            m_undo.push_back(snapshot);
            m_redo.clear();
            m_dragMoved = true;
        }
        const QPointF d = p - m_dragStart;
        if (m_dragVertex >= 0 && m_dragVertex < a->pts.size())
            a->pts[m_dragVertex] = m_dragOrig[m_dragVertex] + d;
        else
            for (int i = 0; i < a->pts.size(); ++i)
                a->pts[i] = m_dragOrig[i] + d;
        emit changed();
        return true;
    }
    return false;
}

bool AnnotationLayer::mouseRelease(const QPointF &p, Qt::MouseButton b, double scale)
{
    if (m_dragId) {
        m_dragId = 0;
        m_dragVertex = -1;
        return true;
    }
    if (m_drawing && b == Qt::LeftButton && isTwoPoint(m_current.type)) {
        m_current.pts.back() = p;
        if (QLineF(m_current.pts[0], m_current.pts[1]).length() * scale < 3) {
            m_drawing = false; // a click without drag: ignore
            emit changed();
            return true;
        }
        finishDrawing();
        return true;
    }
    return m_drawing;
}

bool AnnotationLayer::mouseDoubleClick(const QPointF &p, double scale)
{
    if (m_drawing) {
        if (m_current.type == Annotation::Polyline || m_current.type == Annotation::Polygon) {
            // the double click added a duplicate point; drop the rubber-band point
            if (m_current.pts.size() > 2)
                m_current.pts.pop_back();
        }
        finishDrawing();
        return true;
    }
    if (m_tool == SelectTool) {
        int v;
        const int id = hitTest(p, 6.0 / std::max(0.01, scale), &v);
        const Annotation *a = find(id);
        if (a && a->type == Annotation::Text) {
            emit editTextRequested(id);
            return true;
        }
    }
    return false;
}

bool AnnotationLayer::keyPress(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape && m_drawing) {
        m_drawing = false;
        emit changed();
        return true;
    }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && m_drawing) {
        if (m_current.type != Annotation::Count && m_current.pts.size() > 2)
            m_current.pts.pop_back();
        finishDrawing();
        return true;
    }
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && m_selected && !m_drawing) {
        removeSelected();
        return true;
    }
    return false;
}

void AnnotationLayer::finishDrawing()
{
    if (!m_drawing)
        return;
    m_drawing = false;
    bool valid = true;
    switch (m_current.type) {
    case Annotation::Polyline: valid = m_current.pts.size() >= 2; break;
    case Annotation::Polygon: valid = m_current.pts.size() >= 3; break;
    case Annotation::Angle: valid = m_current.pts.size() == 3; break;
    case Annotation::Count: valid = !m_current.pts.isEmpty(); break;
    default: valid = m_current.pts.size() >= 2; break;
    }
    if (valid) {
        if (m_current.type != Annotation::Count)
            pushUndo();
        m_items.push_back(m_current);
        m_selected = m_current.id;
        emit selectionChanged(m_selected);
    }
    emit changed();
}

void AnnotationLayer::paint(QPainter &p, const QTransform &T, double scale, bool forExport) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QVector<const Annotation *> list;
    for (const auto &a : m_items)
        list.push_back(&a);
    if (m_drawing)
        list.push_back(&m_current);
    const double exportScale = forExport ? std::max(1.0, (m_imageSize.width() > 0 ? m_imageSize.width() : 1600) / 1200.0) : 1.0;

    QFont font = p.font();
    font.setPixelSize(int(13 * exportScale));
    font.setBold(true);
    p.setFont(font);

    auto label = [&](const QPointF &anchor, const QString &text, const QColor &c) {
        if (text.isEmpty())
            return;
        const QFontMetricsF fm(p.font());
        QRectF r(anchor + QPointF(8 * exportScale, -fm.height() - 4 * exportScale),
                 QSizeF(fm.horizontalAdvance(text) + 10 * exportScale, fm.height() + 4 * exportScale));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 160));
        p.drawRoundedRect(r, 3, 3);
        p.setPen(c);
        p.drawText(r, Qt::AlignCenter, text);
    };

    for (const Annotation *ap : list) {
        const Annotation &a = *ap;
        const bool sel = a.id == m_selected && !forExport;
        QPen pen(a.color, a.lineWidth * exportScale);
        pen.setCosmetic(!forExport);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QVector<QPointF> s;
        for (const auto &pt : a.pts)
            s.push_back(T.map(pt));
        const Measurement m = measure(a);
        switch (a.type) {
        case Annotation::Line:
        case Annotation::Arrow:
            if (s.size() >= 2) {
                p.drawLine(s[0], s[1]);
                if (a.type == Annotation::Arrow) {
                    const QLineF l(s[1], s[0]);
                    const double hl = 14 * exportScale * std::max(1.0, a.lineWidth / 2);
                    QLineF h1 = QLineF::fromPolar(hl, l.angle() + 25).translated(s[1]);
                    QLineF h2 = QLineF::fromPolar(hl, l.angle() - 25).translated(s[1]);
                    QPolygonF head({s[1], h1.p2(), h2.p2()});
                    p.setBrush(a.color);
                    p.drawPolygon(head);
                    p.setBrush(Qt::NoBrush);
                } else {
                    // end ticks
                    const QLineF l(s[0], s[1]);
                    for (const QPointF &e : {s[0], s[1]}) {
                        QLineF n = l.normalVector();
                        n.setLength(5 * exportScale);
                        const QPointF d = n.p2() - n.p1();
                        p.drawLine(e - d, e + d);
                    }
                }
                if (a.showMeasurement && a.type == Annotation::Line)
                    label((s[0] + s[1]) / 2, m.summary, a.color);
            }
            break;
        case Annotation::Polyline:
            p.drawPolyline(QPolygonF(s));
            if (a.showMeasurement && !s.isEmpty())
                label(s.back(), m.summary, a.color);
            break;
        case Annotation::Rectangle:
            if (s.size() >= 2) {
                const QRectF r = QRectF(s[0], s[1]).normalized();
                p.drawRect(r);
                if (a.showMeasurement)
                    label(r.topRight(), m.summary, a.color);
            }
            break;
        case Annotation::Ellipse:
            if (s.size() >= 2) {
                const QRectF r = QRectF(s[0], s[1]).normalized();
                p.drawEllipse(r);
                if (a.showMeasurement)
                    label(r.topRight(), m.summary, a.color);
            }
            break;
        case Annotation::Polygon: {
            QColor fill = a.color;
            fill.setAlpha(40);
            p.setBrush(fill);
            p.drawPolygon(QPolygonF(s));
            p.setBrush(Qt::NoBrush);
            if (a.showMeasurement && !s.isEmpty() && (!m_drawing || ap != &m_current))
                label(QPolygonF(s).boundingRect().topRight(), m.summary, a.color);
            break;
        }
        case Annotation::Angle:
            p.drawPolyline(QPolygonF(s));
            if (s.size() >= 3 && a.showMeasurement) {
                const double r = 22 * exportScale;
                const QLineF l1(s[1], s[0]), l2(s[1], s[2]);
                double start = l1.angle(), span = l1.angleTo(l2);
                if (span > 180)
                    span -= 360;
                p.drawArc(QRectF(s[1] - QPointF(r, r), QSizeF(2 * r, 2 * r)), int(start * 16), int(span * 16));
                label(s[1], m.summary, a.color);
            }
            break;
        case Annotation::Text:
            if (!s.isEmpty()) {
                QFont f = p.font();
                f.setPixelSize(int(std::max(10.0, 9.0 * a.lineWidth) * exportScale));
                p.setFont(f);
                const QFontMetricsF fm(f);
                QRectF r(s[0], QSizeF(fm.horizontalAdvance(a.text) + 10 * exportScale, fm.height() + 6 * exportScale));
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 140));
                p.drawRoundedRect(r, 3, 3);
                p.setPen(a.color);
                p.drawText(r, Qt::AlignCenter, a.text);
                p.setFont(font);
                if (sel) {
                    p.setPen(QPen(QColor(0, 180, 255), 1, Qt::DashLine));
                    p.setBrush(Qt::NoBrush);
                    p.drawRect(r.adjusted(-2, -2, 2, 2));
                }
            }
            break;
        case Annotation::Count: {
            const double r = 6 * exportScale * std::max(1.0, a.lineWidth / 2);
            QFont f = p.font();
            f.setPixelSize(int(10 * exportScale));
            p.setFont(f);
            for (int i = 0; i < s.size(); ++i) {
                p.setPen(QPen(Qt::black, 1.5 * exportScale));
                p.setBrush(a.color);
                p.drawEllipse(s[i], r, r);
                p.setPen(a.color);
                p.drawText(s[i] + QPointF(r + 2, -r), QString::number(i + 1));
            }
            p.setFont(font);
            if (!s.isEmpty() && a.showMeasurement)
                label(s.back() + QPointF(0, -14 * exportScale), m.summary, a.color);
            break;
        }
        }
        if (sel && a.type != Annotation::Text) {
            p.setPen(QPen(QColor(0, 180, 255), 1));
            p.setBrush(QColor(255, 255, 255));
            for (const auto &q : s)
                p.drawRect(QRectF(q - QPointF(3.5, 3.5), QSizeF(7, 7)));
        }
    }
    (void)scale;
    p.restore();
}

QJsonArray AnnotationLayer::toJson() const
{
    QJsonArray arr;
    for (const auto &a : m_items) {
        QJsonObject o;
        o["type"] = QString::fromLatin1(typeName(a.type));
        QJsonArray pts;
        for (const auto &p : a.pts)
            pts.append(QJsonArray{p.x(), p.y()});
        o["points"] = pts;
        o["text"] = a.text;
        o["color"] = a.color.name(QColor::HexArgb);
        o["width"] = a.lineWidth;
        o["measure"] = a.showMeasurement;
        const Measurement m = measure(a);
        o["summary"] = m.summary;
        arr.append(o);
    }
    return arr;
}

void AnnotationLayer::fromJson(const QJsonArray &arr)
{
    m_items.clear();
    for (const auto &v : arr) {
        const QJsonObject o = v.toObject();
        Annotation a;
        a.id = m_nextId++;
        a.type = typeFromName(o["type"].toString());
        for (const auto &p : o["points"].toArray()) {
            const QJsonArray xy = p.toArray();
            a.pts.push_back(QPointF(xy.at(0).toDouble(), xy.at(1).toDouble()));
        }
        a.text = o["text"].toString();
        a.color = QColor::fromString(o["color"].toString(QStringLiteral("#ffffdc00")));
        a.lineWidth = o["width"].toDouble(2.0);
        a.showMeasurement = o["measure"].toBool(true);
        m_items.push_back(a);
    }
    m_undo.clear();
    m_redo.clear();
    m_selected = 0;
    emit changed();
}

QString AnnotationLayer::sidecarPath(const QString &imagePath)
{
    return imagePath + QStringLiteral(".annotations.json");
}

bool AnnotationLayer::saveSidecar(const QString &imagePath) const
{
    const QString path = sidecarPath(imagePath);
    if (m_items.isEmpty()) {
        QFile::remove(path);
        return true;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    QJsonObject root;
    root["umPerPixel"] = m_umPerPixel;
    root["annotations"] = toJson();
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

bool AnnotationLayer::loadSidecar(const QString &imagePath)
{
    QFile f(sidecarPath(imagePath));
    if (!f.open(QIODevice::ReadOnly)) {
        m_items.clear();
        m_undo.clear();
        m_redo.clear();
        m_selected = 0;
        emit changed();
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    fromJson(root["annotations"].toArray());
    return true;
}

} // namespace lm
