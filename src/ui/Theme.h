#pragma once

#include <QString>

class QApplication;

namespace lm {

// Applies the application look ("dark" or "light").
void applyTheme(QApplication &app, const QString &name);

} // namespace lm
