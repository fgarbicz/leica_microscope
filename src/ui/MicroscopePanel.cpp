#include "MicroscopePanel.h"

#include "app/Calibration.h"
#include "ui/CollapsibleSection.h"
#include "ui/Icons.h"
#include "ui/Overlays.h"
#include "ui/PanelGroup.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace lm {

MicroscopePanel::MicroscopePanel(MicroscopeConfig *config, QWidget *parent) : QWidget(parent), m_cfg(config)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *group = new PanelGroup(tr("Microscope"), Icon::Microscope, theme().groupMicroscope, this);
    root->addWidget(group);

    auto *sec = group->addSection(tr("Objective & calibration"), Icon::Objective);
    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    m_objective = new QComboBox(this);
    m_objective->setToolTip(tr("Select the objective currently in the light path (Ctrl+1, Ctrl+2, … one per objective)"));
    form->addRow(tr("Objective"), m_objective);
    m_adapter = new QDoubleSpinBox(this);
    m_adapter->setRange(0.1, 3.0);
    m_adapter->setDecimals(3);
    m_adapter->setSingleStep(0.05);
    m_adapter->setSuffix(QStringLiteral(" ×"));
    m_adapter->setToolTip(tr("Magnification of the camera (C-mount) adapter"));
    form->addRow(tr("Camera adapter"), m_adapter);
    sec->contentLayout()->addLayout(form);
    m_scaleInfo = new QLabel(this);
    m_scaleInfo->setObjectName(QStringLiteral("Hint"));
    m_scaleInfo->setWordWrap(true);
    sec->contentLayout()->addWidget(m_scaleInfo);
    auto *remember = new QCheckBox(tr("Remember settings per objective"), this);
    remember->setChecked(m_cfg->rememberSettings);
    remember->setToolTip(tr("Each objective keeps its own exposure, gain and white balance; they are restored when you switch."));
    connect(remember, &QCheckBox::toggled, this, [this](bool on) {
        m_cfg->rememberSettings = on;
        m_cfg->save();
    });
    sec->contentLayout()->addWidget(remember);
    auto *row = new QHBoxLayout;
    auto *edit = new QPushButton(tr("Objectives…"), this);
    edit->setIcon(icon(Icon::Settings, theme().text, 15));
    auto *cal = new QPushButton(tr("Calibrate…"), this);
    cal->setIcon(icon(Icon::Calibrate, theme().text, 15));
    cal->setToolTip(tr("Measure a stage micrometer to calibrate the current objective"));
    row->addWidget(edit);
    row->addWidget(cal);
    sec->contentLayout()->addLayout(row);
    m_section = sec;

    auto *sh = group->addSection(tr("Shading correction"), Icon::Shading, false);
    auto *hint = new QLabel(tr("Move to an empty area of the slide (or remove it), keep the illumination "
                               "as for imaging, then acquire a reference. Stored per objective."), this);
    hint->setObjectName(QStringLiteral("Hint"));
    hint->setWordWrap(true);
    sh->contentLayout()->addWidget(hint);
    auto *acq = new QPushButton(tr("Acquire reference"), this);
    acq->setIcon(icon(Icon::Shading, theme().text, 15));
    sh->contentLayout()->addWidget(acq);
    m_shading = new QCheckBox(tr("Apply shading correction"), this);
    m_shading->setToolTip(tr("Evens out uneven illumination (darker corners), using the reference of this objective"));
    sh->contentLayout()->addWidget(m_shading);
    m_shadingInfo = new QLabel(tr("No reference"), this);
    m_shadingInfo->setObjectName(QStringLiteral("Hint"));
    m_shadingInfo->setWordWrap(true);
    sh->contentLayout()->addWidget(m_shadingInfo);
    m_shadingClear = new QPushButton(tr("Delete reference"), this);
    m_shadingClear->setObjectName(QStringLiteral("DangerButton"));
    m_shadingClear->setIcon(icon(Icon::Trash, theme().danger, 15));
    sh->contentLayout()->addWidget(m_shadingClear);

    connect(m_objective, &QComboBox::activated, this, [this](int i) {
        m_cfg->current = i;
        m_cfg->save();
        refresh();
        emit calibrationChanged();
    });
    connect(m_adapter, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        m_cfg->adapterFactor = v;
        m_cfg->save();
        refresh();
        emit calibrationChanged();
    });
    connect(edit, &QPushButton::clicked, this, &MicroscopePanel::editObjectives);
    connect(cal, &QPushButton::clicked, this, &MicroscopePanel::calibrateRequested);
    connect(acq, &QPushButton::clicked, this, &MicroscopePanel::shadingReferenceRequested);
    connect(m_shading, &QCheckBox::toggled, this, &MicroscopePanel::shadingEnabledChanged);
    connect(m_shadingClear, &QPushButton::clicked, this, &MicroscopePanel::shadingClearRequested);
    refresh();
}

void MicroscopePanel::refresh()
{
    QSignalBlocker b1(m_objective), b2(m_adapter);
    m_objective->clear();
    for (const auto &o : m_cfg->objectives)
        m_objective->addItem(m_cfg->objectiveLabel(o));
    m_objective->setCurrentIndex(m_cfg->current);
    m_adapter->setValue(m_cfg->adapterFactor);
    if (m_cfg->objectives.isEmpty()) {
        m_scaleInfo->clear();
        if (m_section)
            m_section->setSummary(QString());
        return;
    }
    const auto &o = m_cfg->currentObjective();
    const double um = m_cfg->umPerPixel();
    const bool calibrated = o.calibratedUmPerPixel > 0;
    m_scaleInfo->setText(tr("%1 µm/pixel (%2)\nOptical resolution ≈ %3")
                             .arg(um, 0, 'g', 4)
                             .arg(calibrated ? tr("calibrated") : tr("nominal"))
                             .arg(formatLength(m_cfg->resolutionLimitUm())));
    // An uncalibrated objective is the single most common cause of a wrong
    // scale bar, so say so where it cannot be missed.
    m_scaleInfo->setObjectName(QString::fromLatin1(calibrated ? "Hint" : "StatusWarn"));
    m_scaleInfo->style()->unpolish(m_scaleInfo);
    m_scaleInfo->style()->polish(m_scaleInfo);
    // the collapsed section still shows which objective is selected
    if (m_section)
        m_section->setSummary(tr("%1× · %2 µm/px").arg(o.magnification).arg(um, 0, 'g', 3));
}

void MicroscopePanel::setShadingStatus(const QString &text, bool available)
{
    m_shadingInfo->setText(text);
    m_shading->setEnabled(available);
    m_shadingClear->setEnabled(available);
    if (!available) {
        QSignalBlocker b(m_shading);
        m_shading->setChecked(false);
    }
}

bool MicroscopePanel::shadingEnabled() const
{
    return m_shading->isChecked();
}

void MicroscopePanel::setShadingEnabled(bool on)
{
    m_shading->setChecked(on);
}

void MicroscopePanel::editObjectives()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Objectives"));
    dlg.resize(px(720), px(380));
    auto *lay = new QVBoxLayout(&dlg);
    auto *table = new QTableWidget(int(m_cfg->objectives.size()), 5, &dlg);
    table->setHorizontalHeaderLabels({tr("Name"), tr("Magnification"), tr("NA"), tr("Immersion"), tr("Calibrated µm/pixel (0 = use nominal)")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto fill = [&](int r, const Objective &o, int original) {
        auto *nameItem = new QTableWidgetItem(o.name);
        nameItem->setData(Qt::UserRole, original); // index into the original list (-1 = new)
        table->setItem(r, 0, nameItem);
        table->setItem(r, 1, new QTableWidgetItem(QString::number(o.magnification)));
        table->setItem(r, 2, new QTableWidgetItem(QString::number(o.na)));
        table->setItem(r, 3, new QTableWidgetItem(o.immersion));
        table->setItem(r, 4, new QTableWidgetItem(QString::number(o.calibratedUmPerPixel, 'g', 6)));
    };
    for (int r = 0; r < m_cfg->objectives.size(); ++r)
        fill(r, m_cfg->objectives[r], r);
    lay->addWidget(table);
    auto *btns = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add"), &dlg);
    auto *del = new QPushButton(tr("Remove"), &dlg);
    auto *def = new QPushButton(tr("Restore defaults"), &dlg);
    btns->addWidget(add);
    btns->addWidget(del);
    btns->addWidget(def);
    btns->addStretch();
    lay->addLayout(btns);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(bb);
    connect(add, &QPushButton::clicked, &dlg, [&] {
        const int r = table->rowCount();
        table->insertRow(r);
        Objective o;
        o.name = tr("New objective");
        fill(r, o, -1);
    });
    connect(del, &QPushButton::clicked, &dlg, [&] {
        if (table->currentRow() < 0)
            return;
        const QString name = table->item(table->currentRow(), 0) ? table->item(table->currentRow(), 0)->text() : QString();
        if (QMessageBox::question(&dlg, tr("Remove objective"),
                                  tr("Remove %1 and its calibration?").arg(name.isEmpty() ? tr("this objective") : name),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes)
            table->removeRow(table->currentRow());
    });
    connect(def, &QPushButton::clicked, &dlg, [&] {
        if (QMessageBox::question(&dlg, tr("Restore defaults"),
                                  tr("Restore the default objectives? Your calibrations (µm/pixel) and shading "
                                     "references for all objectives are discarded when you press OK."),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
        const auto d = MicroscopeConfig::defaultObjectives();
        table->setRowCount(int(d.size()));
        for (int r = 0; r < d.size(); ++r)
            fill(r, d[r], -1);
    });
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QList<Objective> list;
    for (int r = 0; r < table->rowCount(); ++r) {
        auto txt = [&](int c) { return table->item(r, c) ? table->item(r, c)->text() : QString(); };
        const int original = table->item(r, 0) ? table->item(r, 0)->data(Qt::UserRole).toInt() : -1;
        // start from the original objective so shading, id and stored camera settings survive
        Objective o = original >= 0 && original < m_cfg->objectives.size() ? m_cfg->objectives[original] : Objective();
        o.name = txt(0);
        o.magnification = std::max(0.1, txt(1).toDouble());
        o.na = txt(2).toDouble();
        o.immersion = txt(3);
        o.calibratedUmPerPixel = std::max(0.0, txt(4).toDouble());
        if (o.id.isEmpty())
            o.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
        list.append(o);
    }
    if (list.isEmpty())
        list = MicroscopeConfig::defaultObjectives();
    m_cfg->objectives = list;
    m_cfg->current = std::clamp(m_cfg->current, 0, int(list.size()) - 1);
    m_cfg->save();
    refresh();
    emit calibrationChanged();
}

} // namespace lm
