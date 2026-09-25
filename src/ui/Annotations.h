#pragma once
// Annotation and measurement layer drawn on top of an image. All geometry is
// stored in image pixel coordinates; measurements use the µm/px calibration.

#include <QColor>
#include <QJsonArray>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QTransform>
#include <QVector>

class QPainter;
class QImage;
class QSize;
class QKeyEvent;

namespace lm {

struct Annotation {
    enum Type { Line, Arrow, Polyline, Rectangle, Ellipse, Polygon, Angle, Text, Count };
    int id = 0;
    Type type = Line;
    QVector<QPointF> pts;
    QString text;
    QColor color = QColor(255, 220, 0);
    double lineWidth = 2.0;
    bool showMeasurement = true;
};

// True for annotations that enclose an area (rectangle, ellipse, polygon).
bool isRegion(const Annotation &a);
// Grayscale8 mask (255 inside) of the union of the given region annotations.
QImage regionMask(const QVector<const Annotation *> &regions, QSize size);

struct Measurement {
    double lengthUm = 0;     // line/polyline length or perimeter
    double areaUm2 = 0;
    double angleDeg = 0;
    int count = 0;
    double widthUm = 0, heightUm = 0;
    QString summary;         // formatted
};

class AnnotationLayer : public QObject {
    Q_OBJECT
public:
    enum Tool { SelectTool, LineTool, ArrowTool, PolylineTool, RectTool, EllipseTool, PolygonTool, AngleTool,
                TextTool, CountTool };

    explicit AnnotationLayer(QObject *parent = nullptr);

    void setTool(Tool t);
    Tool tool() const { return m_tool; }
    void setUmPerPixel(double v) { m_umPerPixel = v; emit changed(); }
    double umPerPixel() const { return m_umPerPixel; }
    void setColor(const QColor &c);
    QColor color() const { return m_color; }
    void setLineWidth(double w) { m_lineWidth = w; }
    void setImageSize(QSize s) { m_imageSize = s; }

    const QVector<Annotation> &annotations() const { return m_items; }
    int selectedId() const { return m_selected; }
    void select(int id);
    void clear();
    void removeSelected();
    void addText(const QPointF &pos, const QString &text);
    void setAnnotationText(int id, const QString &text);
    bool undo();
    bool redo();

    Measurement measure(const Annotation &a) const;

    // Interaction in image coordinates; `scale` = screen pixels per image pixel
    // (for hit tolerances). Return true when the event was consumed.
    bool mousePress(const QPointF &p, Qt::MouseButton b, Qt::KeyboardModifiers mods, double scale);
    bool mouseMove(const QPointF &p, Qt::MouseButtons b, double scale);
    bool mouseRelease(const QPointF &p, Qt::MouseButton b, double scale);
    bool mouseDoubleClick(const QPointF &p, double scale);
    bool keyPress(QKeyEvent *e);
    bool isDrawing() const { return m_drawing; }

    // Paint with `toScreen` mapping image -> device coordinates.
    void paint(QPainter &p, const QTransform &toScreen, double scale, bool forExport = false) const;

    QJsonArray toJson() const;
    void fromJson(const QJsonArray &a);
    bool saveSidecar(const QString &imagePath) const;
    bool loadSidecar(const QString &imagePath);
    static QString sidecarPath(const QString &imagePath);

signals:
    void changed();
    void selectionChanged(int id);
    void textRequested(const QPointF &pos);
    void editTextRequested(int id);

private:
    void pushUndo();
    int hitTest(const QPointF &p, double tol, int *vertex) const;
    void finishDrawing();
    Annotation *find(int id);

    QVector<Annotation> m_items;
    QVector<QVector<Annotation>> m_undo, m_redo;
    Tool m_tool = SelectTool;
    QColor m_color = QColor(255, 220, 0);
    double m_lineWidth = 2.0;
    double m_umPerPixel = 0.0;
    QSize m_imageSize;
    int m_nextId = 1;
    int m_selected = 0;
    bool m_drawing = false;
    Annotation m_current;
    QPointF m_hover;
    // dragging
    int m_dragId = 0, m_dragVertex = -1;
    QPointF m_dragStart;
    QVector<QPointF> m_dragOrig;
    bool m_dragMoved = false;
};

} // namespace lm
