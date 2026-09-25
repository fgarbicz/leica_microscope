#pragma once
// Stops the mouse wheel from changing values.
//
// The side panels are tall enough to scroll, and a wheel event over a slider,
// spin box, combo box or tab bar used to change that control instead of
// scrolling the panel — silently altering the exposure, the objective or an
// analysis threshold while the user was only looking for a control further
// down. Values are changed by dragging, typing or the arrow keys; the wheel
// only ever scrolls.
//
// Installed once on the application; it needs no cooperation from the widgets,
// so it also covers the dialogs and any control added later.

#include <QObject>

class QApplication;

namespace lm {

class WheelGuard : public QObject {
    Q_OBJECT
public:
    explicit WheelGuard(QObject *parent = nullptr) : QObject(parent) {}

    // Installs the guard for the lifetime of the application.
    static void install(QApplication &app);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

} // namespace lm
