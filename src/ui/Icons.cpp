#include "Icons.h"

#include "Theme.h"

#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

namespace lm {

namespace {

// Each icon is the body of a 24x24 SVG. "@C" is replaced by the requested
// colour, so one definition serves every theme and state. Strokes are used
// throughout (filled shapes only where a symbol needs a solid dot), which keeps
// the set visually consistent at 14-24 px.
const char *iconBody(Icon which)
{
    switch (which) {
    case Icon::None:
        return "";

    // ---------------- workflow
    case Icon::Acquire: // camera body with a live aperture
        return R"(<path d="M3 8.5a2 2 0 0 1 2-2h2.2l1.3-2h6.4l1.3 2H19a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>
                  <circle cx="12" cy="12.5" r="3.4"/>)";
    case Icon::Browse: // grid of thumbnails
        return R"(<rect x="3.5" y="3.5" width="7" height="7" rx="1.4"/><rect x="13.5" y="3.5" width="7" height="7" rx="1.4"/>
                  <rect x="3.5" y="13.5" width="7" height="7" rx="1.4"/><rect x="13.5" y="13.5" width="7" height="7" rx="1.4"/>)";
    case Icon::Process: // sliders
        return R"(<path d="M4 7h10M18 7h2M4 12h3M11 12h9M4 17h8M16 17h4"/>
                  <circle cx="16" cy="7" r="2"/><circle cx="9" cy="12" r="2"/><circle cx="14" cy="17" r="2"/>)";

    // ---------------- groups and sections
    case Icon::Camera:
        return R"(<path d="M3 8.5a2 2 0 0 1 2-2h2.2l1.3-2h6.4l1.3 2H19a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>
                  <circle cx="12" cy="12.5" r="3"/>)";
    case Icon::Exposure: // sun / brightness
        return R"(<circle cx="12" cy="12" r="4"/>
                  <path d="M12 2.5v2.2M12 19.3v2.2M2.5 12h2.2M19.3 12h2.2M5.3 5.3l1.6 1.6M17.1 17.1l1.6 1.6M18.7 5.3l-1.6 1.6M6.9 17.1l-1.6 1.6"/>)";
    case Icon::Orientation: // flip arrows around an axis
        return R"(<path d="M12 3v18"/><path d="M8 7L4 12l4 5" /><path d="M16 7l4 5-4 5"/>)";
    case Icon::Chip:
        return R"(<rect x="7" y="7" width="10" height="10" rx="1.5"/>
                  <path d="M10 3.5v3M14 3.5v3M10 17.5v3M14 17.5v3M3.5 10h3M3.5 14h3M17.5 10h3M17.5 14h3"/>)";
    case Icon::Microscope:
        return R"(<path d="M6 20h13"/><path d="M9.5 20a5.5 5.5 0 0 0 7.5-5.1"/>
                  <path d="M11 4.2l3.2 1.9a1.2 1.2 0 0 1 .4 1.7l-3 5a1.2 1.2 0 0 1-1.7.4L6.7 11.3a1.2 1.2 0 0 1-.4-1.7l3-5a1.2 1.2 0 0 1 1.7-.4z"/>
                  <path d="M7.6 16.5l2.2-3.6"/>)";
    case Icon::Objective: // lens barrel
        return R"(<path d="M8.5 3.5h7l-.7 5.2 2.2 4.3a2 2 0 0 1-.2 2.1l-2.3 2.9a2 2 0 0 1-1.6.8h-1.8a2 2 0 0 1-1.6-.8l-2.3-2.9a2 2 0 0 1-.2-2.1l2.2-4.3z"/>
                  <path d="M8.1 8.7h7.8"/>)";
    case Icon::Shading: // uneven illumination falling off to the corners
        return R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/>
                  <circle cx="12" cy="12" r="5.2"/><circle cx="12" cy="12" r="2.2"/>)";
    case Icon::Capture: // shutter
        return R"(<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="3.2"/>
                  <path d="M12 3.5v5.3M19.4 16.2l-4.6-2.6M4.6 16.2l4.6-2.6"/>)";
    case Icon::Folder:
        return R"(<path d="M3.5 7.5a1.5 1.5 0 0 1 1.5-1.5h4l2 2.2h8a1.5 1.5 0 0 1 1.5 1.5v8.3a1.5 1.5 0 0 1-1.5 1.5H5a1.5 1.5 0 0 1-1.5-1.5z"/>)";
    case Icon::Multifocus: // stacked planes
        return R"(<path d="M12 3.2l8 3.6-8 3.6-8-3.6z"/><path d="M4 12l8 3.6 8-3.6"/><path d="M4 16.8l8 3.6 8-3.6"/>)";
    case Icon::Stitch: // overlapping tiles
        return R"(<rect x="3.5" y="3.5" width="10" height="10" rx="1.5"/><rect x="10.5" y="10.5" width="10" height="10" rx="1.5"/>)";
    case Icon::Video:
        return R"(<rect x="2.5" y="6.5" width="13" height="11" rx="2"/><path d="M15.5 11l6-3.2v8.4l-6-3.2z"/>)";
    case Icon::Timelapse:
        return R"(<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3.2 2"/>)";
    case Icon::Histogram:
        return R"(<path d="M3.5 20.5V16M7.2 20.5V9M10.9 20.5V4.5M14.6 20.5V11M18.3 20.5V14.5M21.5 20.5h-19"/>)";
    case Icon::Focus: // focus brackets with a centre dot
        return R"(<path d="M3.5 8.5V5a1.5 1.5 0 0 1 1.5-1.5h3.5M15.5 3.5H19A1.5 1.5 0 0 1 20.5 5v3.5
                           M20.5 15.5V19a1.5 1.5 0 0 1-1.5 1.5h-3.5M8.5 20.5H5A1.5 1.5 0 0 1 3.5 19v-3.5"/>
                  <circle cx="12" cy="12" r="2.6"/>)";
    case Icon::Overlay: // layers
        return R"(<path d="M12 3.2l8.2 4.3L12 11.8 3.8 7.5z"/><path d="M4.6 12.2L12 16l7.4-3.8"/><path d="M4.6 16.6L12 20.4l7.4-3.8"/>)";
    case Icon::Info:
        return R"(<circle cx="12" cy="12" r="8.5"/><path d="M12 11v5.5"/><circle cx="12" cy="8" r="0.9" fill="@C" stroke="none"/>)";
    case Icon::Palette:
        return R"(<path d="M12 3.5a8.5 8.5 0 0 0 0 17c1.2 0 1.8-.8 1.8-1.7 0-.9-.6-1.5-.6-2.3 0-.9.7-1.6 1.7-1.6h1.5a3.6 3.6 0 0 0 3.6-3.6C20 6.3 16.5 3.5 12 3.5z"/>
                  <circle cx="8.3" cy="9.2" r="1.15" fill="@C" stroke="none"/><circle cx="12" cy="7.4" r="1.15" fill="@C" stroke="none"/>
                  <circle cx="15.7" cy="9.2" r="1.15" fill="@C" stroke="none"/><circle cx="7.6" cy="13.4" r="1.15" fill="@C" stroke="none"/>)";
    case Icon::Lamp: // lamp with a filter in front
        return R"(<path d="M12 3.5a5.5 5.5 0 0 0-3.2 10v2.2h6.4V13.5A5.5 5.5 0 0 0 12 3.5z"/>
                  <path d="M9.6 18.4h4.8M10.4 20.8h3.2"/>)";
    case Icon::Contrast:
        return R"(<circle cx="12" cy="12" r="8.5"/><path d="M12 3.5a8.5 8.5 0 0 1 0 17z" fill="@C" stroke="none"/>)";
    case Icon::Sharpen: // a hard edge
        return R"(<path d="M12 3.6l8.4 16.8H3.6z"/><path d="M12 8.6v7"/>)";
    case Icon::Measure: // ruler
        return R"(<path d="M3.2 14.6L14.6 3.2l6.2 6.2L9.4 20.8z"/><path d="M7.6 10.2l2 2M10.8 7l2 2M14 3.8l2 2"/>)";
    case Icon::Adjust:
        return R"(<path d="M6 4v6M6 14v6M12 4v10M12 18v2M18 4v2M18 10v10"/>
                  <circle cx="6" cy="12" r="2"/><circle cx="12" cy="16" r="2"/><circle cx="18" cy="8" r="2"/>)";
    case Icon::Ihc: // stained nuclei
        return R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/>
                  <circle cx="8.6" cy="9.8" r="1.9" fill="@C" stroke="none"/><circle cx="15" cy="9" r="1.5"/>
                  <circle cx="11.5" cy="15" r="1.9" fill="@C" stroke="none"/><circle cx="16.6" cy="14.6" r="1.5"/>)";

    // ---------------- actions
    case Icon::Play:
        return R"(<path d="M8 5.2l11 6.8-11 6.8z" fill="@C" stroke="none"/>)";
    case Icon::Stop:
        return R"(<rect x="6.5" y="6.5" width="11" height="11" rx="1.6" fill="@C" stroke="none"/>)";
    case Icon::Freeze: // snowflake
        return R"(<path d="M12 3v18M4.2 7.5l15.6 9M19.8 7.5l-15.6 9"/>
                  <path d="M9.6 4.8L12 7.2l2.4-2.4M9.6 19.2L12 16.8l2.4 2.4"/>)";
    case Icon::Refresh:
        return R"(<path d="M20 12a8 8 0 1 1-2.6-5.9"/><path d="M20.4 4.6v4.2h-4.2"/>)";
    case Icon::Plug:
        return R"(<path d="M9 3.5v4.2M15 3.5v4.2"/><path d="M6.8 7.7h10.4v3.1a5.2 5.2 0 0 1-10.4 0z"/><path d="M12 16v4.5"/>)";
    case Icon::Disconnect:
        return R"(<path d="M9 3.5v4.2M15 3.5v4.2"/><path d="M6.8 7.7h10.4v3.1a5.2 5.2 0 0 1-10.4 0z"/>
                  <path d="M12 16v4.5"/><path d="M3.5 3.5l17 17"/>)";
    case Icon::Reset:
        return R"(<path d="M4 12a8 8 0 1 0 2.6-5.9"/><path d="M3.6 4.6v4.2h4.2"/>)";
    case Icon::Plus:
        return R"(<path d="M12 5.5v13M5.5 12h13"/>)";
    case Icon::Minus:
        return R"(<path d="M5.5 12h13"/>)";
    case Icon::Settings:
        return R"(<circle cx="12" cy="12" r="2.8"/>
                  <path d="M12 3.2l1 2.4 2.5-.7 1 2.3 2.5.5-.4 2.6 2 1.7-1.6 2 1 2.4-2.3 1.2-.2 2.6-2.6-.3-1.6 2-2.3-1.3-2.3 1.3-1.6-2-2.6.3-.2-2.6L2.9 18.4l1-2.4-1.6-2 2-1.7-.4-2.6 2.5-.5 1-2.3 2.5.7z"/>)";
    case Icon::Help:
        return R"(<circle cx="12" cy="12" r="8.5"/><path d="M9.6 9.3a2.5 2.5 0 1 1 3.4 2.3c-.6.3-1 .9-1 1.6v.4"/>
                  <circle cx="12" cy="16.6" r="0.9" fill="@C" stroke="none"/>)";
    case Icon::Compare:
        return R"(<rect x="3.5" y="5.5" width="7.5" height="13" rx="1.5"/><rect x="13" y="5.5" width="7.5" height="13" rx="1.5"/>
                  <path d="M12 3.2v17.6"/>)";
    case Icon::Print:
        return R"(<path d="M7 8.5V3.5h10v5"/><rect x="3.5" y="8.5" width="17" height="7.5" rx="1.5"/>
                  <path d="M7 14h10v6.5H7z"/>)";
    case Icon::Export:
        return R"(<path d="M12 15.5V3.8"/><path d="M8.2 7.6L12 3.8l3.8 3.8"/>
                  <path d="M4.5 14.5v4a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2v-4"/>)";
    case Icon::Save:
        return R"(<path d="M4.5 5.5a1 1 0 0 1 1-1h10.6l3.4 3.4v10.6a1 1 0 0 1-1 1h-13a1 1 0 0 1-1-1z"/>
                  <path d="M8 4.5v5h8v-5"/><rect x="8" y="13" width="8" height="6.5"/>)";
    case Icon::Open:
        return R"(<path d="M3.5 7.5a1.5 1.5 0 0 1 1.5-1.5h4l2 2.2h8a1.5 1.5 0 0 1 1.5 1.5"/>
                  <path d="M3.5 7.8l1.9 10a1.5 1.5 0 0 0 1.5 1.2h10.2a1.5 1.5 0 0 0 1.5-1.2l1.9-8"/>)";
    case Icon::Trash:
        return R"(<path d="M4.5 7h15"/><path d="M9.5 7V4.5h5V7"/>
                  <path d="M6.5 7l1 12a1.5 1.5 0 0 0 1.5 1.4h6a1.5 1.5 0 0 0 1.5-1.4l1-12"/><path d="M10.5 11v6M13.5 11v6"/>)";
    case Icon::ZoomFit:
        return R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/>
                  <path d="M8.5 8.5h-2v2M15.5 8.5h2v2M8.5 15.5h-2v-2M15.5 15.5h2v-2"/>)";
    case Icon::ZoomActual:
        return R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M8.6 9v6M11.4 15h4.2M13.5 9v6"/>)";
    case Icon::ZoomIn:
        return R"(<circle cx="10.5" cy="10.5" r="6.5"/><path d="M15.4 15.4l5 5"/><path d="M10.5 7.5v6M7.5 10.5h6"/>)";
    case Icon::ZoomOut:
        return R"(<circle cx="10.5" cy="10.5" r="6.5"/><path d="M15.4 15.4l5 5"/><path d="M7.5 10.5h6"/>)";
    case Icon::Fullscreen:
        return R"(<path d="M3.5 9V4.5a1 1 0 0 1 1-1H9M15 3.5h4.5a1 1 0 0 1 1 1V9M20.5 15v4.5a1 1 0 0 1-1 1H15M9 20.5H4.5a1 1 0 0 1-1-1V15"/>)";
    case Icon::Calibrate: // stage micrometer scale
        return R"(<path d="M3 16.5h18"/><path d="M4.5 16.5v-4M8 16.5v-6.5M11.5 16.5v-4M15 16.5v-6.5M18.5 16.5v-4"/>
                  <path d="M3 20h18"/>)";
    case Icon::Pick: // eyedropper
        return R"(<path d="M15.6 3.9l4.5 4.5-2.1 2.1-4.5-4.5z"/>
                  <path d="M13.5 6l-8 8-1.6 5.2 5.2-1.6 8-8"/>)";
    case Icon::Wand:
        return R"(<path d="M5 19l9.5-9.5"/><path d="M15.5 4v3.4M20.5 9h-3.4M17.6 5.2l-2.4 2.4M17.6 12.8l-2.4-2.4"/>
                  <path d="M13.2 8.2l2.6 2.6"/>)";
    case Icon::Check:
        return R"(<path d="M4.5 12.8l4.8 4.7L19.5 7"/>)";
    case Icon::Warning:
        return R"(<path d="M12 4.2l8.5 15.3H3.5z"/><path d="M12 9.6v4.4"/>
                  <circle cx="12" cy="16.8" r="0.9" fill="@C" stroke="none"/>)";
    case Icon::Error:
        return R"(<circle cx="12" cy="12" r="8.5"/><path d="M8.8 8.8l6.4 6.4M15.2 8.8l-6.4 6.4"/>)";
    case Icon::ChevronDown:
        return R"(<path d="M6.5 9.5l5.5 5.5 5.5-5.5"/>)";
    case Icon::ChevronRight:
        return R"(<path d="M9.5 6.5l5.5 5.5-5.5 5.5"/>)";
    case Icon::Up: // go to the parent folder
        return R"(<path d="M12 19.5V5.2"/><path d="M5.8 11.4L12 5.2l6.2 6.2"/>)";
    case Icon::Grid:
        return R"(<rect x="3.5" y="4.5" width="17" height="15" rx="1.6"/><path d="M9.2 4.5v15M14.8 4.5v15M3.5 9.5h17M3.5 14.5h17"/>)";
    case Icon::Crosshair:
        return R"(<circle cx="12" cy="12" r="7.5"/><path d="M12 2.5v5M12 16.5v5M2.5 12h5M16.5 12h5"/>)";
    case Icon::ScaleBar:
        return R"(<path d="M3.5 13.5h17"/><path d="M3.5 10.5v6M20.5 10.5v6"/><path d="M8 17.5h8"/>)";
    }
    return "";
}

struct CacheKey {
    Icon which;
    QRgb color;
    int px;
    bool operator==(const CacheKey &o) const { return which == o.which && color == o.color && px == o.px; }
};

size_t qHash(const CacheKey &k, size_t seed = 0)
{
    return qHashMulti(seed, int(k.which), uint(k.color), k.px);
}

QHash<CacheKey, QPixmap> g_cache;

} // namespace

void clearIconCache()
{
    g_cache.clear();
}

QPixmap iconPixmap(Icon which, const QColor &color, int px)
{
    if (which == Icon::None || px <= 0)
        return {};
    const CacheKey key{which, color.rgba(), px};
    if (const auto it = g_cache.constFind(key); it != g_cache.constEnd())
        return it.value();

    QString body = QString::fromLatin1(iconBody(which));
    body.replace(QLatin1String("@C"), color.name());
    // Stroke width shrinks a little at large sizes so big icons do not look
    // heavy and small ones stay legible.
    const double stroke = px <= 16 ? 1.7 : px <= 24 ? 1.6 : 1.45;
    const QString doc = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
                                       "stroke='%1' stroke-width='%2' stroke-linecap='round' "
                                       "stroke-linejoin='round'>%3</svg>")
                            .arg(color.name())
                            .arg(stroke, 0, 'f', 2)
                            .arg(body);

    QSvgRenderer renderer(doc.toUtf8());
    if (!renderer.isValid())
        return {};
    // Device pixel ratio 2 keeps the drawing crisp on HiDPI screens (all three
    // platforms scale, and Qt picks the right representation).
    constexpr qreal kDpr = 2.0;
    QPixmap pm(QSize(px, px) * kDpr);
    pm.setDevicePixelRatio(kDpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&p, QRectF(0, 0, px, px));
    p.end();
    g_cache.insert(key, pm);
    return pm;
}

QIcon icon(Icon which, const QColor &color, int px)
{
    if (which == Icon::None)
        return {};
    QIcon ic;
    ic.addPixmap(iconPixmap(which, color, px), QIcon::Normal);
    // a dimmed copy, so a disabled button does not look merely greyed out by Qt
    QColor off = color;
    off.setAlphaF(0.4f);
    ic.addPixmap(iconPixmap(which, off, px), QIcon::Disabled);
    return ic;
}

QIcon icon(Icon which, int px)
{
    return icon(which, theme().subText, px);
}

} // namespace lm
