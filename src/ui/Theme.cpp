#include "Theme.h"

#include "Icons.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QPalette>
#include <QRegularExpression>
#include <QStyle>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QTabBar>
#include <QLayout>
#include <QToolBar>
#include <QStringList>
#include <QWidget>

#include <algorithm>
#include <iterator>

namespace lm {

namespace {

ThemeColors makeDark()
{
    ThemeColors c;
    c.dark = true;
    c.window = QColor(32, 34, 38);
    c.canvas = QColor(18, 19, 22);
    c.panel = QColor(40, 42, 47);
    c.panelAlt = QColor(46, 48, 54);
    c.header = QColor(52, 55, 61);
    c.border = QColor(64, 67, 74);
    c.text = QColor(228, 230, 235);
    c.subText = QColor(156, 161, 170);
    c.faintText = QColor(112, 117, 126);
    c.accent = QColor(58, 142, 246);
    c.accentHover = QColor(88, 162, 250);
    c.accentPressed = QColor(38, 112, 206);
    c.success = QColor(64, 190, 130);
    c.warning = QColor(234, 168, 62);
    c.danger = QColor(228, 88, 78);
    c.live = QColor(226, 72, 62);
    // group hues: distinguishable, and still readable for the common
    // red-green colour vision deficiencies (they differ in lightness too)
    c.groupCamera = QColor(88, 162, 250);     // blue
    c.groupMicroscope = QColor(158, 142, 244); // violet
    c.groupCapture = QColor(72, 194, 160);     // teal
    c.groupImage = QColor(238, 176, 76);       // amber
    c.groupAdjust = QColor(236, 130, 178);     // pink
    c.groupOverlay = QColor(126, 190, 96);     // green
    return c;
}

ThemeColors makeLight()
{
    ThemeColors c;
    c.dark = false;
    c.window = QColor(240, 242, 245);
    c.canvas = QColor(56, 58, 62); // still dark: a bright surround skews how a stain looks
    c.panel = QColor(255, 255, 255);
    c.panelAlt = QColor(246, 247, 249);
    c.header = QColor(233, 236, 240);
    c.border = QColor(208, 213, 221);
    c.text = QColor(28, 32, 38);
    c.subText = QColor(92, 99, 108);
    c.faintText = QColor(150, 156, 165);
    c.accent = QColor(26, 115, 232);
    c.accentHover = QColor(48, 133, 244);
    c.accentPressed = QColor(18, 92, 190);
    c.success = QColor(26, 145, 92);
    c.warning = QColor(178, 116, 12);
    c.danger = QColor(196, 54, 46);
    c.live = QColor(210, 50, 42);
    c.groupCamera = QColor(26, 115, 232);
    c.groupMicroscope = QColor(112, 88, 220);
    c.groupCapture = QColor(20, 148, 122);
    c.groupImage = QColor(176, 116, 14);
    c.groupAdjust = QColor(196, 72, 130);
    c.groupOverlay = QColor(76, 140, 52);
    return c;
}

ThemeColors g_colors = makeDark();
int g_uiScale = 100;

// The sizes Settings offers. Anything in between (set by hand or inherited from
// an older version) still works; these are the steps the menu walks through.
const int kScaleSteps[] = {75, 85, 100, 115, 130, 150, 175, 200};

// Fusion computes icon sizes, check box indicators and scroll bar widths from
// its own pixel metrics. Scaling those here means a larger interface grows in
// every dimension, not just in text.
class ScaledStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override
    {
        const int base = QProxyStyle::pixelMetric(metric, option, widget);
        switch (metric) {
        case PM_SmallIconSize:
        case PM_ButtonIconSize:
        case PM_ToolBarIconSize:
        case PM_LargeIconSize:
        case PM_ListViewIconSize:
        case PM_TabBarIconSize:
        case PM_MessageBoxIconSize:
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
        case PM_ExclusiveIndicatorWidth:
        case PM_ExclusiveIndicatorHeight:
        case PM_ScrollBarExtent:
        case PM_SliderThickness:
        case PM_SliderLength:
        case PM_TitleBarHeight:
        case PM_MenuButtonIndicator:
            return px(base);
        default:
            return base;
        }
    }
};

// The interface font. A named family is chosen only when it exists, so no
// platform falls back to something unintended.
QFont interfaceFont()
{
    const QStringList preferred = {
#ifdef Q_OS_MACOS
        QStringLiteral("SF Pro Text"), QStringLiteral("Helvetica Neue"),
#elif defined(Q_OS_WIN)
        QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
#else
        QStringLiteral("Inter"), QStringLiteral("Cantarell"), QStringLiteral("Ubuntu"),
        QStringLiteral("DejaVu Sans"), QStringLiteral("Noto Sans"),
#endif
    };
    const QStringList available = QFontDatabase::families();
    QFont f;
    for (const QString &name : preferred) {
        if (available.contains(name, Qt::CaseInsensitive)) {
            f.setFamily(name);
            break;
        }
    }
    // fractional sizes keep the steps smooth (13 pt at 115% is 14.95, not 15)
    f.setPointSizeF(baseFontPointSize() * g_uiScale / 100.0);
    return f;
}

QString rgba(const QColor &c, double alpha)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha, 0, 'f', 3);
}

} // namespace

const ThemeColors &theme()
{
    return g_colors;
}

int uiScale()
{
    return g_uiScale;
}

int minUiScale()
{
    return kScaleSteps[0];
}

int maxUiScale()
{
    return kScaleSteps[std::size(kScaleSteps) - 1];
}

int nextUiScale(int percent, int direction)
{
    const int n = int(std::size(kScaleSteps));
    if (direction > 0) {
        for (int i = 0; i < n; ++i)
            if (kScaleSteps[i] > percent)
                return kScaleSteps[i];
        return kScaleSteps[n - 1];
    }
    for (int i = n - 1; i >= 0; --i)
        if (kScaleSteps[i] < percent)
            return kScaleSteps[i];
    return kScaleSteps[0];
}

int px(int deviceIndependentPixels)
{
    return (deviceIndependentPixels * g_uiScale + 50) / 100;
}

QSize iconSize(int base)
{
    const int n = px(base);
    return QSize(n, n);
}

void fitToolBar(QToolBar *bar, int availableWidth)
{
    if (!bar || availableWidth <= 0)
        return;
    // Measure with the labels on. The hint is cached, so the layout has to be
    // invalidated after each change for the next measurement to be honest.
    const auto hintWith = [bar](Qt::ToolButtonStyle style) {
        if (bar->toolButtonStyle() != style) {
            bar->setToolButtonStyle(style);
            if (bar->layout())
                bar->layout()->invalidate();
            bar->layout()->activate();
        }
        return bar->sizeHint().width();
    };
    if (hintWith(Qt::ToolButtonTextBesideIcon) > availableWidth)
        hintWith(Qt::ToolButtonIconOnly); // the tooltips still say what each one does
}

void applyUiScaleTo(QWidget *root)
{
    if (!root)
        return;
    // Qt picks up the new font, palette and style sheet by itself. Icon sizes
    // are a per-widget property, so they are refreshed here; the icons hold
    // several resolutions (see Icons.cpp) and the right one is chosen.
    const auto scaleIcons = [](QWidget *w) {
        if (auto *tb = qobject_cast<QToolBar *>(w))
            tb->setIconSize(iconSize(16));
        else if (auto *tabs = qobject_cast<QTabBar *>(w))
            tabs->setIconSize(iconSize(16));
        else if (auto *b = qobject_cast<QAbstractButton *>(w)) {
            // keep the relative size a widget asked for (a section chevron is
            // smaller than a tool bar button)
            const int base = w->property("lmIconBase").isValid() ? w->property("lmIconBase").toInt() : 15;
            b->setIconSize(iconSize(base));
        }
    };
    scaleIcons(root);
    for (QWidget *w : root->findChildren<QWidget *>())
        scaleIcons(w);
}

int baseFontPointSize()
{
    // Windows renders 9 pt at the size macOS renders 13 px; matching them by
    // eye keeps the three builds looking like one application.
#ifdef Q_OS_MACOS
    return 13;
#elif defined(Q_OS_WIN)
    return 9;
#else
    return 10;
#endif
}

void applyTheme(QApplication &app, const QString &name, int scalePercent)
{
    g_uiScale = std::clamp(scalePercent, 75, 200);
    // Fusion on every platform: the native styles disagree about padding,
    // check box size and group boxes, which is exactly what must not differ
    // between the Windows, macOS and Linux builds.
    // Fusion on every platform, wrapped so its pixel metrics follow the
    // interface size. setStyle() takes ownership of the proxy.
    app.setStyle(new ScaledStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
    const bool dark = name != QLatin1String("light");
    g_colors = dark ? makeDark() : makeLight();
    const ThemeColors &c = g_colors;
    clearIconCache(); // icons carry the old colours
    app.setFont(interfaceFont());

    QPalette pal;
    pal.setColor(QPalette::Window, c.window);
    pal.setColor(QPalette::WindowText, c.text);
    pal.setColor(QPalette::Base, dark ? c.canvas.lighter(130) : QColor(255, 255, 255));
    pal.setColor(QPalette::AlternateBase, c.panelAlt);
    pal.setColor(QPalette::ToolTipBase, c.header);
    pal.setColor(QPalette::ToolTipText, c.text);
    pal.setColor(QPalette::Text, c.text);
    pal.setColor(QPalette::Button, c.header);
    pal.setColor(QPalette::ButtonText, c.text);
    pal.setColor(QPalette::BrightText, dark ? Qt::white : Qt::black);
    pal.setColor(QPalette::Highlight, c.accent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::Link, c.accentHover);
    pal.setColor(QPalette::LinkVisited, c.groupMicroscope);
    pal.setColor(QPalette::Mid, c.border);
    pal.setColor(QPalette::Dark, dark ? QColor(22, 23, 26) : QColor(180, 186, 194));
    pal.setColor(QPalette::Light, dark ? QColor(78, 82, 89) : QColor(255, 255, 255));
    pal.setColor(QPalette::PlaceholderText, c.faintText);
    pal.setColor(QPalette::Disabled, QPalette::Text, c.faintText);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, c.faintText);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, c.faintText);
    pal.setColor(QPalette::Disabled, QPalette::Base, c.panel);
    app.setPalette(pal);

    // Style sheet. Colours come from the palette above via %TOKEN%
    // substitution, so there is one place to change them. A narrow literal
    // converted at run time, not QStringLiteral: that would make it a UTF-16
    // literal of twice the size, past MSVC's 16 KB limit for one literal (C2026).
    QString css = QString::fromUtf8(R"(
/* ---------- frame ---------- */
QMainWindow::separator { background: %BORDER%; width: 3px; height: 3px; }
QMainWindow::separator:hover { background: %ACCENT%; }
QToolTip { background: %HEADER%; color: %TEXT%; border: 1px solid %BORDER%; padding: 4px 6px; }

/* ---------- workflow bar ---------- */
QWidget#TopBar { background: %WINDOW%; border-bottom: 1px solid %BORDER%; }
QLabel#AppTitle { font-weight: 700; font-size: %TITLEPT%pt; color: %TEXT%; padding: 0 6px 0 12px; }
QLabel#AppSubtitle { color: %SUB%; padding: 0 12px 0 0; }
QTabBar#WorkflowTabs { qproperty-drawBase: 0; }
QTabBar#WorkflowTabs::tab { background: transparent; color: %SUB%; padding: 9px 20px; font-weight: 600;
                            border: none; border-bottom: 2px solid transparent; min-width: 84px; }
QTabBar#WorkflowTabs::tab:selected { color: %TEXT%; border-bottom: 2px solid %ACCENT%; }
QTabBar#WorkflowTabs::tab:hover:!selected { color: %TEXT%; background: %HOVERWASH%; }

/* ---------- side panels ---------- */
QScrollArea#PanelScroll { border: none; background: %PANEL%; }
QScrollArea#PanelScroll > QWidget > QWidget { background: %PANEL%; }
QWidget#PanelContent { background: %PANEL%; }

/* group header: the top level of the panel hierarchy */
QWidget#PanelGroup { background: %WINDOW%; }
QLabel#GroupTitle { color: %SUB%; font-size: %SMALLPT%pt; font-weight: 700; letter-spacing: 1px; }

/* section: the second level */
QWidget#SectionHeader { background: %PANEL%; }
QWidget#SectionHeader:hover { background: %HOVERWASH%; }
QLabel#SectionTitle { font-weight: 600; color: %TEXT%; }
QLabel#SectionSummary { color: %SUB%; font-size: %SMALLPT%pt; }
QWidget#SectionContent { background: %PANEL%; }
QFrame#SectionRule { background: %BORDER%; border: none; }

/* controls inside a section: the third level */
QLabel#ControlLabel { color: %SUB%; }
QLabel#ValueLabel { color: %TEXT%; font-weight: 600; }
QLabel#Hint { color: %SUB%; font-size: %SMALLPT%pt; }
QLabel#StatusOk { color: %SUCCESS%; font-weight: 600; }
QLabel#StatusWarn { color: %WARNING%; font-weight: 600; }
QLabel#StatusError { color: %DANGER%; font-weight: 600; }
QToolButton#ResetButton { color: %SUB%; padding: 0px 2px; border: none; border-radius: 3px; }
QToolButton#ResetButton:hover { color: %TEXT%; background: %HOVERWASH%; }

/* ---------- buttons ---------- */
QPushButton { padding: 5px 12px; border: 1px solid %BORDER%; border-radius: 5px; background: %HEADER%; color: %TEXT%; }
QPushButton:hover { border-color: %ACCENT%; background: %HOVERWASH2%; }
QPushButton:pressed { background: %ACCENTPRESSED%; color: white; border-color: %ACCENTPRESSED%; }
QPushButton:checked { background: %ACCENT%; color: white; border-color: %ACCENT%; }
QPushButton:disabled { color: %FAINT%; border-color: %BORDER%; background: transparent; }
QPushButton:focus { border-color: %ACCENT%; }
QPushButton#PrimaryButton { background: %ACCENT%; color: white; border: none; font-weight: 700;
                            font-size: %BIGPT%pt; padding: 10px; border-radius: 6px; }
QPushButton#PrimaryButton:hover { background: %ACCENTHOVER%; }
QPushButton#PrimaryButton:pressed { background: %ACCENTPRESSED%; }
QPushButton#PrimaryButton:disabled { background: %DISABLEDFILL%; color: %FAINT%; }
QPushButton#LiveButton:checked { background: %LIVE%; border-color: %LIVE%; }
QPushButton#DangerButton { color: %DANGER%; }
QPushButton#DangerButton:hover { background: %DANGERWASH%; border-color: %DANGER%; }
QToolButton { border: none; border-radius: 4px; padding: 3px; color: %TEXT%; }
QToolButton:hover { background: %HOVERWASH2%; }
QToolButton:pressed { background: %ACCENTPRESSED%; }
QToolButton:checked { background: %ACCENT%; color: white; }
QToolButton::menu-indicator { image: none; }
/* the chevron is part of a heading, not a button in its own right: it must
   never fill with the accent colour */
QToolButton#SectionChevron, QToolButton#SectionChevron:checked, QToolButton#SectionChevron:pressed,
QToolButton#SectionChevron:hover { background: transparent; border: none; }

/* ---------- inputs ---------- */
QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit {
    background: %FIELD%; border: 1px solid %BORDER%; border-radius: 5px; padding: 3px 6px; color: %TEXT%;
    selection-background-color: %ACCENT%; selection-color: white; }
QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: %BORDERSTRONG%; }
QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: %ACCENT%; }
QComboBox:disabled, QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: %FAINT%; background: transparent; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: %PANEL%; border: 1px solid %BORDER%; selection-background-color: %ACCENT%;
                              selection-color: white; padding: 2px; }
QCheckBox, QRadioButton { spacing: 7px; color: %TEXT%; padding: 1px 0; }
QCheckBox:disabled, QRadioButton:disabled { color: %FAINT%; }
QCheckBox::indicator, QRadioButton::indicator { width: 15px; height: 15px; }
QCheckBox::indicator:unchecked { border: 1px solid %BORDERSTRONG%; border-radius: 3px; background: %FIELD%; }
QCheckBox::indicator:unchecked:hover { border-color: %ACCENT%; }
QCheckBox::indicator:checked { border: 1px solid %ACCENT%; border-radius: 3px; background: %ACCENT%; }
QRadioButton::indicator:unchecked { border: 1px solid %BORDERSTRONG%; border-radius: 8px; background: %FIELD%; }
QRadioButton::indicator:checked { border: 4px solid %ACCENT%; border-radius: 8px; background: %FIELD%; }

/* ---------- sliders ---------- */
QSlider::groove:horizontal { height: 4px; background: %BORDER%; border-radius: 2px; }
QSlider::sub-page:horizontal { background: %ACCENT%; border-radius: 2px; }
QSlider::handle:horizontal { background: %TEXT%; width: 13px; height: 13px; margin: -5px 0; border-radius: 7px; }
QSlider::handle:horizontal:hover { background: %ACCENT%; }
QSlider::groove:horizontal:disabled { background: %BORDER%; }
QSlider::sub-page:horizontal:disabled { background: %FAINT%; }
QSlider::handle:horizontal:disabled { background: %FAINT%; }

/* ---------- scrollbars ---------- */
QScrollBar:vertical { background: transparent; width: 11px; margin: 0; }
QScrollBar::handle:vertical { background: %SCROLLTHUMB%; border-radius: 5px; min-height: 28px; margin: 2px; }
QScrollBar::handle:vertical:hover { background: %SUB%; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 0; }
QScrollBar::handle:horizontal { background: %SCROLLTHUMB%; border-radius: 5px; min-width: 28px; margin: 2px; }
QScrollBar::handle:horizontal:hover { background: %SUB%; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

/* ---------- lists, tables, tabs ---------- */
QListWidget#Gallery { background: %WINDOW%; border: none; border-top: 1px solid %BORDER%; }
QListWidget#Gallery::item { border-radius: 5px; padding: 3px; }
QListWidget#Gallery::item:hover { background: %HOVERWASH2%; }
QListWidget#Gallery::item:selected { background: %ACCENT%; }
QHeaderView::section { background: %HEADER%; color: %SUB%; border: none; border-right: 1px solid %BORDER%;
                       border-bottom: 1px solid %BORDER%; padding: 5px; font-weight: 600; }
QTableWidget, QTreeView, QListView { gridline-color: %BORDER%; background: %PANEL%; border: 1px solid %BORDER%;
                                     border-radius: 5px; }
QTableWidget::item:selected, QTreeView::item:selected, QListView::item:selected { background: %ACCENT%; color: white; }
QTabWidget::pane { border: 1px solid %BORDER%; border-radius: 5px; top: -1px; }
QTabBar::tab { background: %WINDOW%; color: %SUB%; padding: 6px 14px; border: 1px solid %BORDER%;
               border-bottom: none; border-top-left-radius: 5px; border-top-right-radius: 5px; }
QTabBar::tab:selected { background: %PANEL%; color: %TEXT%; }

/* ---------- groups, bars, dialogs ---------- */
QGroupBox { border: 1px solid %BORDER%; border-radius: 6px; margin-top: 11px; padding: 8px 6px 6px 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 9px; padding: 0 5px; color: %SUB%; font-weight: 600; }
QToolBar { background: %PANEL%; border: none; border-bottom: 1px solid %BORDER%; spacing: 3px; padding: 4px; }
QToolBar QToolButton { padding: 5px; }
QToolBar::separator { background: %BORDER%; width: 1px; margin: 4px 5px; }
QStatusBar { background: %WINDOW%; color: %SUB%; border-top: 1px solid %BORDER%; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: %SUB%; padding: 0 9px; }
QProgressBar { border: none; border-radius: 4px; background: %BORDER%; text-align: center; height: 15px; color: %TEXT%; }
QProgressBar::chunk { background: %ACCENT%; border-radius: 4px; }
QMenuBar { background: %WINDOW%; color: %TEXT%; }
QMenuBar::item { padding: 5px 9px; background: transparent; }
QMenuBar::item:selected { background: %HOVERWASH2%; border-radius: 4px; }
QMenu { background: %PANEL%; border: 1px solid %BORDER%; padding: 5px; }
QMenu::item { padding: 5px 26px 5px 26px; border-radius: 4px; }
QMenu::item:selected { background: %ACCENT%; color: white; }
QMenu::separator { height: 1px; background: %BORDER%; margin: 4px 8px; }
QMenu::icon { left: 6px; }
QDialog { background: %WINDOW%; }
QSplitter::handle { background: %BORDER%; }
QSplitter::handle:hover { background: %ACCENT%; }
)");

    struct Token {
        const char *name;
        QString value;
    };
    const QList<Token> tokens = {
        {"%WINDOW%", c.window.name()},
        {"%PANEL%", c.panel.name()},
        {"%PANELALT%", c.panelAlt.name()},
        {"%HEADER%", c.header.name()},
        {"%BORDER%", c.border.name()},
        {"%BORDERSTRONG%", (dark ? c.border.lighter(135) : c.border.darker(112)).name()},
        {"%TEXT%", c.text.name()},
        {"%SUB%", c.subText.name()},
        {"%FAINT%", c.faintText.name()},
        {"%ACCENT%", c.accent.name()},
        {"%ACCENTHOVER%", c.accentHover.name()},
        {"%ACCENTPRESSED%", c.accentPressed.name()},
        {"%SUCCESS%", c.success.name()},
        {"%WARNING%", c.warning.name()},
        {"%DANGER%", c.danger.name()},
        {"%LIVE%", c.live.name()},
        {"%FIELD%", (dark ? c.canvas.lighter(135) : QColor(255, 255, 255)).name()},
        {"%HOVERWASH%", rgba(c.text, 0.05)},
        {"%HOVERWASH2%", rgba(c.accent, dark ? 0.16 : 0.10)},
        {"%DANGERWASH%", rgba(c.danger, dark ? 0.18 : 0.10)},
        {"%DISABLEDFILL%", rgba(c.accent, 0.22)},
        {"%SCROLLTHUMB%", rgba(c.text, 0.22)},
        // font sizes follow the interface size, like the application font
        {"%SMALLPT%", QString::number((baseFontPointSize() - 1) * g_uiScale / 100.0, 'f', 1)},
        {"%TITLEPT%", QString::number((baseFontPointSize() + 2) * g_uiScale / 100.0, 'f', 1)},
        {"%BIGPT%", QString::number((baseFontPointSize() + 1) * g_uiScale / 100.0, 'f', 1)},
    };
    for (const Token &t : tokens)
        css.replace(QLatin1String(t.name), t.value);

    // Every length in the sheet above is written for 100%; scale them all in one
    // pass, so a padding or a corner radius never has to be remembered
    // separately. Lengths are the only px values in the sheet.
    if (g_uiScale != 100) {
        static const QRegularExpression lengths(QStringLiteral("(-?\\d+)px"));
        QString scaled;
        scaled.reserve(css.size() + 64);
        qsizetype last = 0;
        auto it = lengths.globalMatch(css);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            scaled += QStringView(css).mid(last, m.capturedStart() - last);
            const int value = m.captured(1).toInt();
            // a hairline stays a hairline until the interface is much larger
            const int out = value < 0 ? -px(-value) : px(value);
            scaled += QString::number(out) + QStringLiteral("px");
            last = m.capturedEnd();
        }
        scaled += QStringView(css).mid(last);
        css = scaled;
    }
    app.setStyleSheet(css);
}

} // namespace lm
