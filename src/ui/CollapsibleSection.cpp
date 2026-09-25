#include "CollapsibleSection.h"

#include "ui/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace lm {

CollapsibleSection::CollapsibleSection(const QString &title, QWidget *parent, bool expanded, Icon iconId,
                                       const QColor &accent)
    : QWidget(parent), m_accent(accent.isValid() ? accent : theme().subText)
{
    setObjectName(QStringLiteral("CollapsibleSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("SectionHeader"));
    auto *hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(8, 1, 8, 1);
    hl->setSpacing(4);

    // The disclosure chevron sits at the far left, ahead of the icon and the
    // title, and toggles the section like the title does.
    m_chevron = new QToolButton(m_header);
    m_chevron->setObjectName(QStringLiteral("SectionChevron"));
    m_chevron->setAutoRaise(true);
    m_chevron->setCursor(Qt::PointingHandCursor);
    m_chevron->setFocusPolicy(Qt::NoFocus);
    m_chevron->setIconSize(QSize(13, 13));
    hl->addWidget(m_chevron);

    m_button = new QToolButton(m_header);
    m_button->setObjectName(QStringLiteral("SectionButton"));
    // '&' in a title (e.g. "Brightness & contrast") would become a mnemonic
    m_button->setText(QString(title).replace(QLatin1Char('&'), QStringLiteral("&&")));
    m_button->setCheckable(true);
    m_button->setAutoRaise(true);
    m_button->setCursor(Qt::PointingHandCursor);
    m_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // the section's own icon, drawn in the enclosing group's colour
    if (iconId != Icon::None) {
        m_button->setIcon(icon(iconId, m_accent, 15));
        m_button->setIconSize(QSize(15, 15));
        m_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    } else {
        m_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
    hl->addWidget(m_button, 1);
    connect(m_chevron, &QToolButton::clicked, m_button, &QToolButton::toggle);

    m_summary = new QLabel(m_header);
    m_summary->setObjectName(QStringLiteral("Hint"));
    m_summary->hide();
    hl->addWidget(m_summary);
    outer->addWidget(m_header);

    m_content = new QWidget(this);
    m_content->setObjectName(QStringLiteral("SectionContent"));
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(12, 4, 12, 10);
    m_layout->setSpacing(7);
    outer->addWidget(m_content);

    auto *rule = new QFrame(this);
    rule->setObjectName(QStringLiteral("SectionRule"));
    rule->setFrameShape(QFrame::NoFrame);
    rule->setFixedHeight(1);
    outer->addWidget(rule);

    const QString key = QStringLiteral("ui/section/") + title;
    expanded = QSettings().value(key, expanded).toBool();
    connect(m_button, &QToolButton::toggled, this, [this, key](bool on) {
        m_content->setVisible(on);
        updateChevron(on);
        m_summary->setVisible(!on && !m_summary->text().isEmpty());
        QSettings().setValue(key, on);
    });
    m_button->setChecked(expanded);
    m_content->setVisible(expanded);
    updateChevron(expanded);
}

void CollapsibleSection::updateChevron(bool expanded)
{
    // A chevron from the shared icon set, so the panels look the same on every
    // platform (Qt's own arrow primitives differ between styles).
    m_chevron->setIcon(icon(expanded ? Icon::ChevronDown : Icon::ChevronRight, theme().subText, 13));
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

void CollapsibleSection::setSummary(const QString &text)
{
    m_summary->setText(text);
    m_summary->setVisible(!isExpanded() && !text.isEmpty());
}

} // namespace lm
