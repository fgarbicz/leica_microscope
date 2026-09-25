#pragma once
// A titled, collapsible section: the middle level of a side panel (see
// PanelGroup.h). Its expanded state is remembered between sessions.

#include "ui/Icons.h"

#include <QColor>
#include <QWidget>

class QToolButton;
class QVBoxLayout;
class QLabel;

namespace lm {

class CollapsibleSection : public QWidget {
    Q_OBJECT
public:
    // `accent` tints the icon; pass the enclosing PanelGroup's colour (an
    // invalid colour falls back to the theme's secondary text colour).
    explicit CollapsibleSection(const QString &title, QWidget *parent = nullptr, bool expanded = true,
                                Icon icon = Icon::None, const QColor &accent = QColor());

    QVBoxLayout *contentLayout() const { return m_layout; }
    QWidget *content() const { return m_content; }
    void setExpanded(bool on);
    bool isExpanded() const;
    // A widget placed at the right side of the header (e.g. a reset button)
    void setHeaderWidget(QWidget *w);
    // A short status shown in the header, so a collapsed section still says
    // what it is set to ("20x, 0.31 µm/px").
    void setSummary(const QString &text);

private:
    void updateChevron(bool expanded);

    QToolButton *m_chevron;
    QToolButton *m_button;
    QLabel *m_summary;
    QWidget *m_content;
    QVBoxLayout *m_layout;
    QWidget *m_header;
    QColor m_accent;
};

} // namespace lm
