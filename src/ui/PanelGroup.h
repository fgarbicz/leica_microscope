#pragma once
// The top level of a side panel.
//
// The panels used to be a flat stack of a dozen collapsible sections, which
// made it hard to see what belonged together. A panel is now:
//
//   PanelGroup   "CAMERA"          <- what part of the instrument (coloured, with an icon)
//     Section    "Device"          <- collapsible
//       controls "Exposure time"   <- label + slider + value
//     Section    "Exposure"
//
// The group is a heading, not another collapsible level: only one thing folds,
// so there is no doubt about what a click does.

#include "ui/Icons.h"

#include <QColor>
#include <QWidget>

class QVBoxLayout;
class QLabel;

namespace lm {

class CollapsibleSection;

class PanelGroup : public QWidget {
    Q_OBJECT
public:
    // `color` tints the icon and the rule under the title; pass one of the
    // ThemeColors::group* colours.
    PanelGroup(const QString &title, Icon icon, const QColor &color, QWidget *parent = nullptr);

    // Adds a section to this group and returns it, so callers read as
    //   auto *s = group->addSection(tr("Device"), Icon::Camera);
    CollapsibleSection *addSection(const QString &title, Icon icon = Icon::None, bool expanded = true);
    // For a section built elsewhere.
    void addSection(CollapsibleSection *section);
    void addWidget(QWidget *w);

    const QColor &color() const { return m_color; }

private:
    QColor m_color;
    QVBoxLayout *m_sections;
    QLabel *m_icon;
    QLabel *m_title;
    Icon m_iconId;
};

} // namespace lm
