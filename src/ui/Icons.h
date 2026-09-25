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

// Themed icon (uses ThemeColors::subText, dimmed for the disabled state).
// `size` is the size at 100% interface scale; the icon carries larger
// resolutions too, so it stays sharp when the interface is enlarged.
QIcon icon(Icon which, int size = 16);
// Icon in an explicit colour, e.g. a panel group's colour.
QIcon icon(Icon which, const QColor &color, int size = 16);
// Single pixmap at exactly this size, for painting into a widget.
QPixmap iconPixmap(Icon which, const QColor &color, int size);

// Called by applyTheme(): drops the cache so icons are redrawn in the new
// colours.
void clearIconCache();

} // namespace lm
