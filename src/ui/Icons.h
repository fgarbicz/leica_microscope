#pragma once
// The icon set. Icons are stroke drawings defined in code as SVG fragments and
// rendered in a colour taken from the theme, so one set serves dark and light
// mode, any size and any accent colour, and Windows, macOS and Linux show
// exactly the same symbols.

#include <QIcon>
#include <QPixmap>

class QColor;

namespace lm {

enum class Icon {
    None,
    // workflow
    Acquire,
    Browse,
    Process,
    // panel groups and sections
    Camera,
    Exposure,
    Orientation,
    Chip,          // camera details / hardware
    Microscope,
    Objective,
    Shading,
    Capture,
    Folder,
    Multifocus,
    Stitch,
    Video,
    Timelapse,
    Histogram,
    Focus,
    Overlay,
    Info,
    Palette,
    Lamp,          // light filter
    Contrast,
    Sharpen,
    Measure,
    Adjust,
    Ihc,
    // actions
    Play,
    Stop,
    Freeze,
    Refresh,
    Plug,
    Disconnect,
    Reset,
    Plus,
    Minus,
    Settings,
    Help,
    Compare,
    Print,
    Export,
    Save,
    Open,
    Trash,
    ZoomFit,
    ZoomActual,
    ZoomIn,
    ZoomOut,
    Fullscreen,
    Calibrate,
    Pick,          // eyedropper
    Wand,          // automatic
    Check,
    Warning,
    Error,
    ChevronDown,
    ChevronRight,
    Up,
    Grid,
    Crosshair,
    ScaleBar,
};

// Themed icon (uses ThemeColors::text, dimmed for the disabled state).
QIcon icon(Icon which, int px = 16);
// Icon in an explicit colour, e.g. a panel group's colour.
QIcon icon(Icon which, const QColor &color, int px = 16);
// Single pixmap, for painting into a widget.
QPixmap iconPixmap(Icon which, const QColor &color, int px);

// Called by applyTheme(): drops the cache so icons are redrawn in the new
// colours.
void clearIconCache();

} // namespace lm
