#pragma once
// The application look. One palette drives everything: the Qt palette, the
// style sheet, the icon colours and the custom-painted widgets (image view
// overlays, histogram), so dark and light mode stay consistent and the three
// platforms look the same.

#include <QColor>
#include <QSize>
#include <QString>

class QApplication;
class QWidget;

namespace lm {

// Semantic colours. Widgets and icons ask for a role, never for a literal
// colour, so a palette change reaches every part of the interface.
struct ThemeColors {
    bool dark = true;

    QColor window;      // behind everything
    QColor canvas;      // the image area (darkest, so the specimen dominates)
    QColor panel;       // side panel background
    QColor panelAlt;    // alternating rows, nested content
    QColor header;      // section headers, buttons, toolbars
    QColor border;      // hairlines
    QColor text;        // primary text
    QColor subText;     // labels, hints
    QColor faintText;   // disabled

    QColor accent;      // interactive / selected
    QColor accentHover;
    QColor accentPressed;

    QColor success;     // ready, connected, in focus
    QColor warning;     // needs attention (uncalibrated, clipping)
    QColor danger;      // errors, destructive actions
    QColor live;        // recording / live indicator

    // One colour per functional group of the side panels, so a group is
    // recognisable before its title is read.
    QColor groupCamera;
    QColor groupMicroscope;
    QColor groupCapture;
    QColor groupImage;
    QColor groupAdjust;
    QColor groupOverlay;
};

// The palette in use. Valid before applyTheme() too (it returns the dark set).
const ThemeColors &theme();

// Applies "dark" or "light" (anything else means dark) at `scalePercent`
// (75-200; 100 is the platform default size). Everything the interface draws
// scales with it: text, control heights, padding, icons and spacing.
void applyTheme(QApplication &app, const QString &name, int scalePercent = 100);

// The scale in use.
int uiScale();
// The lower and upper ends offered in Settings, and the steps between them.
int minUiScale();
int maxUiScale();
// The next step up (or down) from `percent`, for View > Interface size.
int nextUiScale(int percent, int direction);

// A length in device-independent pixels, scaled by the interface size. Widgets
// use it instead of literal pixel values so they grow with the text.
int px(int deviceIndependentPixels);

// Base point size for the interface, chosen per platform: the same nominal
// size renders differently on Windows, macOS and Linux, so it is corrected
// here instead of leaving the three looking unlike each other. Unscaled.
int baseFontPointSize();

// Icon edge length for a control, scaled. `base` is the size at 100%.
QSize iconSize(int base);

// Applies the current interface size to a widget tree that already exists:
// Qt takes the new font, palette and style sheet by itself, but the icon size
// of buttons, tool bars and tab bars has to be set. Called after a live change.
void applyUiScaleTo(QWidget *root);

} // namespace lm
