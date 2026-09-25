#include "WheelGuard.h"

#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QScrollBar>
#include <QTabBar>
#include <QWheelEvent>

namespace lm {

namespace {

// Controls whose value the wheel must not change.
bool isValueControl(QWidget *w)
{
    if (!w)
        return false;
    // A scroll bar IS a slider, but scrolling it is the whole point.
    if (qobject_cast<QScrollBar *>(w))
        return false;
    return qobject_cast<QAbstractSlider *>(w) || qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QComboBox *>(w)
           || qobject_cast<QTabBar *>(w);
}

// The scroll area that should scroll instead. Stops at a value control's own
// viewport (a combo box popup scrolls normally) and at the widget itself.
QAbstractScrollArea *scrollableAncestor(QWidget *w)
{
    for (QWidget *p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget()) {
        if (auto *sa = qobject_cast<QAbstractScrollArea *>(p)) {
            QScrollBar *bar = sa->verticalScrollBar();
            if (bar && bar->minimum() != bar->maximum())
                return sa;
            // that one cannot scroll; keep looking outwards
        }
    }
    return nullptr;
}

} // namespace

void WheelGuard::install(QApplication &app)
{
    app.installEventFilter(new WheelGuard(&app));
}

bool WheelGuard::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::Wheel)
        return QObject::eventFilter(watched, event);
    auto *w = qobject_cast<QWidget *>(watched);
    if (!isValueControl(w))
        return QObject::eventFilter(watched, event);

    // Hand the scroll to the enclosing panel, so the wheel still does the one
    // thing the user expects it to do.
    auto *we = static_cast<QWheelEvent *>(event);
    if (QAbstractScrollArea *sa = scrollableAncestor(w)) {
        QWidget *viewport = sa->viewport();
        // via global coordinates: the control is a descendant of the viewport,
        // so mapFrom() would have its arguments the wrong way round
        const QPointF pos = viewport->mapFromGlobal(we->globalPosition().toPoint());
        QWheelEvent forwarded(pos, we->globalPosition(), we->pixelDelta(), we->angleDelta(), we->buttons(),
                              we->modifiers(), we->phase(), we->inverted(), we->source());
        QApplication::sendEvent(viewport, &forwarded);
    }
    return true; // never let the control see it
}

} // namespace lm
