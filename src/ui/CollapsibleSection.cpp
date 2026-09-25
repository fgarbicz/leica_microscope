#include "CollapsibleSection.h"

#include <QHBoxLayout>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace lm {

CollapsibleSection::CollapsibleSection(const QString &title, QWidget *parent, bool expanded) : QWidget(parent)
{
    setObjectName(QStringLiteral("CollapsibleSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("SectionHeader"));
    auto *hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(4, 2, 6, 2);
    m_button = new QToolButton(m_header);
    m_button->setObjectName(QStringLiteral("SectionButton"));
    m_button->setText(QString(title).replace(QLatin1Char('&'), QStringLiteral("&&")));
    m_button->setCheckable(true);
    m_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_button->setAutoRaise(true);
    m_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    hl->addWidget(m_button);
    outer->addWidget(m_header);

    m_content = new QWidget(this);
    m_content->setObjectName(QStringLiteral("SectionContent"));
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(10, 6, 10, 10);
    m_layout->setSpacing(6);
    outer->addWidget(m_content);

    const QString key = QStringLiteral("ui/section/") + title;
    expanded = QSettings().value(key, expanded).toBool();
    connect(m_button, &QToolButton::toggled, this, [this, key](bool on) {
        m_content->setVisible(on);
        m_button->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        QSettings().setValue(key, on);
    });
    m_button->setChecked(expanded);
    m_content->setVisible(expanded);
    m_button->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
}

void CollapsibleSection::setExpanded(bool on)
{
    m_button->setChecked(on);
}

bool CollapsibleSection::isExpanded() const
{
    return m_button->isChecked();
}

void CollapsibleSection::setHeaderWidget(QWidget *w)
{
    w->setParent(m_header);
    static_cast<QHBoxLayout *>(m_header->layout())->addWidget(w);
}

} // namespace lm
