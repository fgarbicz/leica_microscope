#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace lm {

void applyTheme(QApplication &app, const QString &name)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    const bool dark = name != QLatin1String("light");
    QPalette pal;
    const QColor accent(45, 140, 235);
    if (dark) {
        const QColor window(37, 38, 41), base(28, 29, 31), alt(45, 46, 50), text(222, 224, 228), mid(70, 72, 78);
        pal.setColor(QPalette::Window, window);
        pal.setColor(QPalette::WindowText, text);
        pal.setColor(QPalette::Base, base);
        pal.setColor(QPalette::AlternateBase, alt);
        pal.setColor(QPalette::ToolTipBase, QColor(50, 52, 56));
        pal.setColor(QPalette::ToolTipText, text);
        pal.setColor(QPalette::Text, text);
        pal.setColor(QPalette::Button, QColor(50, 52, 57));
        pal.setColor(QPalette::ButtonText, text);
        pal.setColor(QPalette::BrightText, Qt::white);
        pal.setColor(QPalette::Highlight, accent);
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::Link, accent.lighter(120));
        pal.setColor(QPalette::Mid, mid);
        pal.setColor(QPalette::Dark, QColor(20, 20, 22));
        pal.setColor(QPalette::Light, QColor(80, 82, 88));
        pal.setColor(QPalette::PlaceholderText, QColor(135, 138, 145));
        pal.setColor(QPalette::Disabled, QPalette::Text, QColor(110, 112, 118));
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(110, 112, 118));
        pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(110, 112, 118));
    } else {
        pal = QApplication::style()->standardPalette();
        pal.setColor(QPalette::Highlight, accent);
        pal.setColor(QPalette::Window, QColor(243, 244, 246));
        pal.setColor(QPalette::Base, QColor(255, 255, 255));
        pal.setColor(QPalette::AlternateBase, QColor(236, 238, 241));
    }
    app.setPalette(pal);

    const QString panel = dark ? QStringLiteral("#2a2b2f") : QStringLiteral("#ffffff");
    const QString header = dark ? QStringLiteral("#303238") : QStringLiteral("#e8eaee");
    const QString border = dark ? QStringLiteral("#3b3d43") : QStringLiteral("#d0d4da");
    const QString text = dark ? QStringLiteral("#dfe1e5") : QStringLiteral("#1f2328");
    const QString sub = dark ? QStringLiteral("#9a9ea6") : QStringLiteral("#5a6068");
    const QString tab = dark ? QStringLiteral("#1f2023") : QStringLiteral("#dde1e6");

    QString css = QStringLiteral(R"(
QWidget { font-size: 9pt; }
QMainWindow::separator { background: %BORDER%; width: 3px; height: 3px; }
QDockWidget { titlebar-close-icon: none; }
QDockWidget::title { background: %HEADER%; padding: 5px 8px; font-weight: 600; color: %TEXT%; }
QScrollArea#PanelScroll { border: none; background: %PANEL%; }
QWidget#PanelContent { background: %PANEL%; }
QWidget#SectionHeader { background: %HEADER%; border-top: 1px solid %BORDER%; }
QToolButton#SectionButton { font-weight: 600; color: %TEXT%; padding: 4px 2px; border: none; text-align: left; }
QWidget#SectionContent { background: %PANEL%; }
QLabel#ControlLabel { color: %SUB%; }
QLabel#ValueLabel { color: %TEXT%; font-weight: 600; }
QLabel#Hint { color: %SUB%; font-size: 8pt; }
QToolButton#ResetButton { color: %SUB%; padding: 0px 2px; }
QPushButton { padding: 5px 12px; border: 1px solid %BORDER%; border-radius: 4px; background: %HEADER%; }
QPushButton:hover { border-color: #2d8ceb; }
QPushButton:pressed { background: #2d8ceb; color: white; }
QPushButton:checked { background: #2d8ceb; color: white; border-color: #2d8ceb; }
QPushButton:disabled { color: #6e7076; }
QPushButton#PrimaryButton { background: #2d8ceb; color: white; border: none; font-weight: 700; font-size: 11pt; padding: 10px; border-radius: 6px; }
QPushButton#PrimaryButton:hover { background: #3c9af5; }
QPushButton#PrimaryButton:pressed { background: #1f73c7; }
QPushButton#PrimaryButton:disabled { background: #3b4a5c; color: #9aa3ad; }
QPushButton#LiveButton:checked { background: #d9453b; border-color: #d9453b; }
QTabBar#WorkflowTabs::tab { background: %TAB%; color: %SUB%; padding: 8px 26px; font-weight: 700; font-size: 10pt;
                            border: none; border-bottom: 3px solid transparent; min-width: 90px; }
QTabBar#WorkflowTabs::tab:selected { color: %TEXT%; border-bottom: 3px solid #2d8ceb; background: %PANEL%; }
QTabBar#WorkflowTabs::tab:hover { color: %TEXT%; }
QWidget#TopBar { background: %TAB%; border-bottom: 1px solid %BORDER%; }
QLabel#AppTitle { font-weight: 800; font-size: 11pt; color: %TEXT%; padding: 0 14px; }
QStatusBar { background: %TAB%; color: %SUB%; }
QStatusBar QLabel { color: %SUB%; padding: 0 8px; }
QToolBar { background: %HEADER%; border: none; spacing: 2px; padding: 2px; }
QToolBar QToolButton { padding: 4px; border-radius: 4px; }
QToolBar QToolButton:checked { background: #2d8ceb; color: white; }
QListWidget#Gallery { background: %TAB%; border: none; }
QListWidget#Gallery::item { border-radius: 4px; padding: 3px; }
QListWidget#Gallery::item:selected { background: #2d8ceb; }
QGroupBox { border: 1px solid %BORDER%; border-radius: 5px; margin-top: 10px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: %SUB%; }
QHeaderView::section { background: %HEADER%; border: none; border-right: 1px solid %BORDER%; padding: 4px; }
QTableWidget, QTreeView, QListView { gridline-color: %BORDER%; }
QSlider::groove:horizontal { height: 4px; background: %BORDER%; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #2d8ceb; border-radius: 2px; }
QSlider::handle:horizontal { background: %TEXT%; width: 12px; height: 12px; margin: -5px 0; border-radius: 6px; }
QSlider::handle:horizontal:hover { background: #ffffff; }
QProgressBar { border: 1px solid %BORDER%; border-radius: 3px; text-align: center; height: 14px; }
QProgressBar::chunk { background: #2d8ceb; }
)");
    css.replace(QStringLiteral("%PANEL%"), panel)
        .replace(QStringLiteral("%HEADER%"), header)
        .replace(QStringLiteral("%BORDER%"), border)
        .replace(QStringLiteral("%TEXT%"), text)
        .replace(QStringLiteral("%SUB%"), sub)
        .replace(QStringLiteral("%TAB%"), tab);
    app.setStyleSheet(css);
}

} // namespace lm
