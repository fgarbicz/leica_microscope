#include "CollapsibleSection.h"

#include "ui/Theme.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace lm {

CollapsibleSection::CollapsibleSection(const QString &title, QWidget *parent, bool expanded, Icon iconId,
                                       const QColor &accent)
    : QWidget(parent), m_accent(accent.isValid() ? accent : theme().subText), m_iconId(iconId),
      m_settingsKey(QStringLiteral("ui/section/") + title)
{
    setObjectName(QStringLiteral("CollapsibleSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("SectionHeader"));
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->installEventFilter(this); // a click anywhere on it folds the section
    auto *hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(px(8), px(3), px(8), px(3));
    hl->setSpacing(px(5));

    // The disclosure chevron. It is a button so it is reachable from the
    // keyboard; the rest of the header is a click target too.
    m_chevron = new QToolButton(m_header);
    m_chevron->setObjectName(QStringLiteral("SectionChevron"));
    m_chevron->setAutoRaise(true);
    m_chevron->setCursor(Qt::PointingHandCursor);
    m_chevron->setProperty("lmIconBase", 13); // read by applyUiScaleTo()
    m_chevron->setIconSize(iconSize(13));
    m_chevron->setToolTip(title);
    hl->addWidget(m_chevron);
    connect(m_chevron, &QToolButton::clicked, this, [this] { setExpanded(!m_expanded); });

    if (iconId != Icon::None) {
        m_iconLabel = new QLabel(m_header);
        m_iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        updateIcon();
        hl->addWidget(m_iconLabel);
    }

    m_title = new QLabel(title, m_header);
    m_title->setObjectName(QStringLiteral("SectionTitle"));
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents); // clicks reach the header
    m_title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hl->addWidget(m_title);

    // The summary takes the room the title leaves and is cut short with "…" when
    // it does not fit. A plain label asks for its whole text, and a long one (a
    // camera's full name) widened the panel past its scroll area, cutting off
    // the right edge of every control in it.
    m_summary = new QLabel(m_header);
    m_summary->setObjectName(QStringLiteral("SectionSummary"));
    m_summary->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_summary->setMinimumWidth(0);
    m_summary->installEventFilter(this);
    m_summary->hide();
    hl->addWidget(m_summary, 1000);
    // keeps the title on the left while there is no summary (a hidden widget
    // takes no room); with one, the summary's far larger stretch takes nearly all
    hl->addStretch(1);
    outer->addWidget(m_header);

    m_content = new QWidget(this);
    m_content->setObjectName(QStringLiteral("SectionContent"));
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(px(12), px(4), px(12), px(10));
    m_layout->setSpacing(px(7));
    outer->addWidget(m_content);

    auto *rule = new QFrame(this);
    rule->setObjectName(QStringLiteral("SectionRule"));
    rule->setFrameShape(QFrame::NoFrame);
    rule->setFixedHeight(px(1));
    outer->addWidget(rule);

    m_expanded = QSettings().value(m_settingsKey, expanded).toBool();
    m_content->setVisible(m_expanded);
    updateChevron();
}

bool CollapsibleSection::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_summary && event->type() == QEvent::Resize)
        elideSummary();
    if (watched == m_header && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && m_header->rect().contains(me->position().toPoint())) {
            setExpanded(!m_expanded);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CollapsibleSection::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // the interface size or the theme changed: the drawn icons carry the old ones
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) {
        updateChevron();
        updateIcon();
    }
}

void CollapsibleSection::updateChevron()
{
    // A chevron from the shared icon set, so the panels look the same on every
    // platform (Qt's own arrow primitives differ between styles).
    m_chevron->setIcon(icon(m_expanded ? Icon::ChevronDown : Icon::ChevronRight, theme().subText, 13));
}

void CollapsibleSection::updateIcon()
{
    if (m_iconLabel)
        m_iconLabel->setPixmap(iconPixmap(m_iconId, m_accent, px(15)));
}

void CollapsibleSection::setExpanded(bool on)
{
    if (on == m_expanded && m_content->isVisible() == on)
        return;
    m_expanded = on;
    m_content->setVisible(on);
    updateChevron();
    m_summary->setVisible(!on && !m_summaryText.isEmpty());
    QSettings().setValue(m_settingsKey, on);
}

void CollapsibleSection::setHeaderWidget(QWidget *w)
{
    w->setParent(m_header);
    static_cast<QHBoxLayout *>(m_header->layout())->addWidget(w);
}

void CollapsibleSection::setSummary(const QString &text)
{
    m_summaryText = text;
    m_summary->setToolTip(text);
    elideSummary();
    m_summary->setVisible(!m_expanded && !text.isEmpty());
}

void CollapsibleSection::elideSummary()
{
    m_summary->setText(m_summary->fontMetrics().elidedText(m_summaryText, Qt::ElideRight, m_summary->width()));
}

} // namespace lm
