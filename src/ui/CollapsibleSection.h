#pragma once
// A titled, collapsible section: the middle level of a side panel (see
// PanelGroup.h). Its expanded state is remembered between sessions.
//
// The whole header is the click target. The title is a plain label rather than
// the text of a tool button, because a tool button with text only centres it
// however the style sheet is written, which left the sections on the Process
// page with their titles in the middle.

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
    bool isExpanded() const { return m_expanded; }
    // A widget placed at the right side of the header (e.g. a reset button)
    void setHeaderWidget(QWidget *w);
    // A short status shown in the header, so a collapsed section still says
    // what it is set to ("20x, 0.31 µm/px").
    void setSummary(const QString &text);

protected:
    // the header is clickable as a whole, and the icon is redrawn when the
    // interface size changes
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateChevron();
    void updateIcon();

    QToolButton *m_chevron;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_title;
    QLabel *m_summary;
    QWidget *m_content;
    QVBoxLayout *m_layout;
    QWidget *m_header;
    QColor m_accent;
    Icon m_iconId = Icon::None;
    QString m_settingsKey;
    bool m_expanded = true;
};

} // namespace lm
