#pragma once
// A titled section that can be collapsed, used to build the side panels.

#include <QWidget>

class QToolButton;
class QVBoxLayout;

namespace lm {

class CollapsibleSection : public QWidget {
    Q_OBJECT
public:
    explicit CollapsibleSection(const QString &title, QWidget *parent = nullptr, bool expanded = true);
    QVBoxLayout *contentLayout() const { return m_layout; }
    QWidget *content() const { return m_content; }
    void setExpanded(bool on);
    bool isExpanded() const;
    // A widget placed at the right side of the header (e.g. a reset button)
    void setHeaderWidget(QWidget *w);

private:
    QToolButton *m_button;
    QWidget *m_content;
    QVBoxLayout *m_layout;
    QWidget *m_header;
};

} // namespace lm
