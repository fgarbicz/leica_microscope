#include "PanelGroup.h"

#include "ui/CollapsibleSection.h"
#include "ui/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace lm {

PanelGroup::PanelGroup(const QString &title, Icon iconId, const QColor &color, QWidget *parent)
    : QWidget(parent), m_color(color), m_iconId(iconId)
{
    setObjectName(QStringLiteral("PanelGroup"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("PanelGroupHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(10, 9, 10, 5);
    hl->setSpacing(7);
    m_icon = new QLabel(header);
    m_icon->setPixmap(iconPixmap(iconId, color, 15));
    hl->addWidget(m_icon);
    m_title = new QLabel(title.toUpper(), header);
    m_title->setObjectName(QStringLiteral("GroupTitle"));
    hl->addWidget(m_title);
    hl->addStretch();
    outer->addWidget(header);

    // A 2 px rule in the group's colour: the panel's structure is visible even
    // when every section is collapsed.
    auto *rule = new QFrame(this);
    rule->setFrameShape(QFrame::NoFrame);
    rule->setFixedHeight(2);
    rule->setStyleSheet(QStringLiteral("background: %1;").arg(color.name()));
    outer->addWidget(rule);

    auto *body = new QWidget(this);
    body->setObjectName(QStringLiteral("SectionContent"));
    m_sections = new QVBoxLayout(body);
    m_sections->setContentsMargins(0, 0, 0, 0);
    m_sections->setSpacing(0);
    outer->addWidget(body);
}

CollapsibleSection *PanelGroup::addSection(const QString &title, Icon iconId, bool expanded)
{
    auto *s = new CollapsibleSection(title, this, expanded, iconId, m_color);
    m_sections->addWidget(s);
    return s;
}

void PanelGroup::addSection(CollapsibleSection *section)
{
    section->setParent(this);
    m_sections->addWidget(section);
}

void PanelGroup::addWidget(QWidget *w)
{
    w->setParent(this);
    m_sections->addWidget(w);
}

} // namespace lm
