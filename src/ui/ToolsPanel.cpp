#include "ToolsPanel.h"

#include "app/AppSettings.h"
#include "ui/CollapsibleSection.h"
#include "ui/HistogramWidget.h"
#include "ui/Overlays.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace lm {

ToolsPanel::ToolsPanel(QWidget *parent) : QWidget(parent)
{
    auto &O = AppSettings::instance().overlays;
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *hs = new CollapsibleSection(tr("Histogram"), this);
    m_hist = new HistogramWidget(this);
    m_hist->setMinimumHeight(130);
    hs->contentLayout()->addWidget(m_hist);
    auto *hHint = new QLabel(tr("Drag the handles to set black and white points."), this);
    hHint->setObjectName(QStringLiteral("Hint"));
    hs->contentLayout()->addWidget(hHint);
    root->addWidget(hs);

    auto *fs = new CollapsibleSection(tr("Focus"), this);
    m_focus = new QLabel(this);
    m_focus->setObjectName(QStringLiteral("ValueLabel"));
    fs->contentLayout()->addWidget(m_focus);
    auto *fa = new QCheckBox(tr("Show focus assistant"), this);
    fa->setChecked(O.focusAssist);
    fs->contentLayout()->addWidget(fa);
    auto *fRow = new QHBoxLayout;
    auto *region = new QPushButton(tr("Focus region…"), this);
    auto *resetPeak = new QPushButton(tr("Reset peak"), this);
    fRow->addWidget(region);
    fRow->addWidget(resetPeak);
    fs->contentLayout()->addLayout(fRow);
    root->addWidget(fs);

    auto *os = new CollapsibleSection(tr("Overlays"), this);
    auto *scale = new QCheckBox(tr("Scale bar"), this);
    scale->setChecked(O.scaleBar);
    auto *grid = new QCheckBox(tr("Grid"), this);
    grid->setChecked(O.grid);
    auto *cross = new QCheckBox(tr("Crosshair"), this);
    cross->setChecked(O.crosshair);
    auto *clip = new QCheckBox(tr("Show over/under exposure"), this);
    clip->setChecked(O.clipping);
    clip->setToolTip(tr("Saturated pixels red, black pixels blue"));
    for (auto *c : {scale, grid, cross, clip})
        os->contentLayout()->addWidget(c);
    auto *of = new QFormLayout;
    of->setContentsMargins(0, 4, 0, 0);
    auto *pos = new QComboBox(this);
    pos->addItems({tr("Top left"), tr("Top right"), tr("Bottom left"), tr("Bottom right")});
    pos->setCurrentIndex(O.scaleBarPosition);
    of->addRow(tr("Scale bar position"), pos);
    auto *len = new QDoubleSpinBox(this);
    len->setRange(0, 100000);
    len->setDecimals(1);
    len->setSuffix(tr(" µm"));
    len->setSpecialValueText(tr("Automatic"));
    len->setValue(O.scaleBarLengthUm);
    of->addRow(tr("Scale bar length"), len);
    auto *color = new QPushButton(this);
    auto setBtnColor = [color](const QColor &c) {
        color->setStyleSheet(QStringLiteral("background:%1; min-height:16px;").arg(c.name()));
    };
    setBtnColor(O.scaleBarColor);
    of->addRow(tr("Scale bar colour"), color);
    auto *bg = new QCheckBox(tr("Background box"), this);
    bg->setChecked(O.scaleBarBackground);
    of->addRow(QString(), bg);
    auto *div = new QSpinBox(this);
    div->setRange(2, 20);
    div->setValue(O.gridDivisions);
    of->addRow(tr("Grid divisions"), div);
    os->contentLayout()->addLayout(of);
    root->addWidget(os);

    auto *is = new CollapsibleSection(tr("Information"), this);
    m_info = new QLabel(this);
    m_info->setObjectName(QStringLiteral("Hint"));
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    is->contentLayout()->addWidget(m_info);
    m_pixel = new QLabel(this);
    m_pixel->setObjectName(QStringLiteral("Hint"));
    is->contentLayout()->addWidget(m_pixel);
    root->addWidget(is);
    root->addStretch();

    auto changed = [this] {
        AppSettings::instance().save();
        emit overlaysChanged();
    };
    connect(scale, &QCheckBox::toggled, this, [changed](bool on) { AppSettings::instance().overlays.scaleBar = on; changed(); });
    connect(grid, &QCheckBox::toggled, this, [changed](bool on) { AppSettings::instance().overlays.grid = on; changed(); });
    connect(cross, &QCheckBox::toggled, this, [changed](bool on) { AppSettings::instance().overlays.crosshair = on; changed(); });
    connect(clip, &QCheckBox::toggled, this, [changed](bool on) { AppSettings::instance().overlays.clipping = on; changed(); });
    connect(fa, &QCheckBox::toggled, this, [changed](bool on) { AppSettings::instance().overlays.focusAssist = on; changed(); });
    connect(pos, &QComboBox::activated, this, [changed](int i) { AppSettings::instance().overlays.scaleBarPosition = i; changed(); });
    connect(len, &QDoubleSpinBox::valueChanged, this, [changed](double v) { AppSettings::instance().overlays.scaleBarLengthUm = v; changed(); });
    connect(bg, &QCheckBox::toggled, this, [changed](bool on) { AppSettings::instance().overlays.scaleBarBackground = on; changed(); });
    connect(div, &QSpinBox::valueChanged, this, [changed](int v) { AppSettings::instance().overlays.gridDivisions = v; changed(); });
    connect(color, &QPushButton::clicked, this, [this, changed, setBtnColor] {
        const QColor c = QColorDialog::getColor(AppSettings::instance().overlays.scaleBarColor, this, tr("Scale bar colour"));
        if (c.isValid()) {
            AppSettings::instance().overlays.scaleBarColor = c;
            setBtnColor(c);
            changed();
        }
    });
    connect(m_hist, &HistogramWidget::levelsChanged, this, &ToolsPanel::levelsChanged);
    connect(region, &QPushButton::clicked, this, &ToolsPanel::focusRegionRequested);
    connect(resetPeak, &QPushButton::clicked, this, [this] { m_focusPeak = 0; });
}

void ToolsPanel::setStats(const LiveStats &s, double umPerPixel)
{
    m_hist->setHistogram(s.histogram);
    m_focusPeak = std::max(m_focusPeak, s.focus);
    m_focus->setText(tr("%1   (peak %2)").arg(s.focus, 0, 'f', 1).arg(m_focusPeak, 0, 'f', 1));
    QString info = tr("Image %1 × %2 px\nCamera %3 fps, display %4 fps\nExposure level %5%, saturated %6%")
                       .arg(s.width)
                       .arg(s.height)
                       .arg(s.fps, 0, 'f', 1)
                       .arg(s.displayFps, 0, 'f', 1)
                       .arg(s.meanLevel * 100, 0, 'f', 1)
                       .arg(s.saturated * 100, 0, 'f', 2);
    if (umPerPixel > 0)
        info += tr("\nField of view %1 × %2").arg(formatLength(s.width * umPerPixel), formatLength(s.height * umPerPixel));
    m_info->setText(info);
}

void ToolsPanel::setPixelInfo(const QString &text)
{
    m_pixel->setText(text);
}

void ToolsPanel::setLevels(double black, double white)
{
    m_hist->setLevels(black, white);
}

} // namespace lm
