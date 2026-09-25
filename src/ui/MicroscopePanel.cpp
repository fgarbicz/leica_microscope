#include "MicroscopePanel.h"

#include "app/Calibration.h"
#include "ui/CollapsibleSection.h"
#include "ui/Overlays.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace lm {

MicroscopePanel::MicroscopePanel(MicroscopeConfig *config, QWidget *parent) : QWidget(parent), m_cfg(config)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *sec = new CollapsibleSection(tr("Microscope"), this);
    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    m_objective = new QComboBox(this);
    m_objective->setToolTip(tr("Select the objective currently in the light path (Ctrl+1 … Ctrl+6)"));
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
    auto *remember = new QCheckBox(tr("Remember exposure && white balance per objective"), this);
    remember->setChecked(m_cfg->rememberSettings);
    remember->setToolTip(tr("Each objective keeps its own exposure, gain and white balance; they are restored when you switch."));
    connect(remember, &QCheckBox::toggled, this, [this](bool on) {
        m_cfg->rememberSettings = on;
        m_cfg->save();
    });
    sec->contentLayout()->addWidget(remember);
    auto *row = new QHBoxLayout;
    auto *edit = new QPushButton(tr("Objectives…"), this);
    auto *cal = new QPushButton(tr("Calibrate…"), this);
    cal->setToolTip(tr("Measure a stage micrometer to calibrate the current objective"));
    row->addWidget(edit);
    row->addWidget(cal);
    sec->contentLayout()->addLayout(row);
    root->addWidget(sec);

    auto *sh = new CollapsibleSection(tr("Shading correction"), this, false);
    auto *hint = new QLabel(tr("Move to an empty area of the slide (or remove it), keep the illumination "
                               "as for imaging, then acquire a reference. Stored per objective."), this);
    hint->setObjectName(QStringLiteral("Hint"));
    hint->setWordWrap(true);
    sh->contentLayout()->addWidget(hint);
    auto *acq = new QPushButton(tr("Acquire reference"), this);
    sh->contentLayout()->addWidget(acq);
    m_shading = new QCheckBox(tr("Apply shading correction"), this);
    sh->contentLayout()->addWidget(m_shading);
    m_shadingInfo = new QLabel(tr("No reference"), this);
    m_shadingInfo->setObjectName(QStringLiteral("Hint"));
    m_shadingInfo->setWordWrap(true);
    sh->contentLayout()->addWidget(m_shadingInfo);
    m_shadingClear = new QPushButton(tr("Delete reference"), this);
    sh->contentLayout()->addWidget(m_shadingClear);
    root->addWidget(sh);

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
        return;
    }
    const auto &o = m_cfg->currentObjective();
    const double um = m_cfg->umPerPixel();
    m_scaleInfo->setText(tr("%1 µm/pixel (%2)\nOptical resolution ≈ %3")
                             .arg(um, 0, 'g', 4)
                             .arg(o.calibratedUmPerPixel > 0 ? tr("calibrated") : tr("nominal"))
                             .arg(formatLength(m_cfg->resolutionLimitUm())));
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
    dlg.resize(720, 380);
    auto *lay = new QVBoxLayout(&dlg);
    auto *table = new QTableWidget(int(m_cfg->objectives.size()), 5, &dlg);
    table->setHorizontalHeaderLabels({tr("Name"), tr("Magnification"), tr("NA"), tr("Immersion"), tr("Calibrated µm/px (0 = nominal)")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto fill = [&](int r, const Objective &o) {
        table->setItem(r, 0, new QTableWidgetItem(o.name));
        table->setItem(r, 1, new QTableWidgetItem(QString::number(o.magnification)));
        table->setItem(r, 2, new QTableWidgetItem(QString::number(o.na)));
        table->setItem(r, 3, new QTableWidgetItem(o.immersion));
        table->setItem(r, 4, new QTableWidgetItem(QString::number(o.calibratedUmPerPixel, 'g', 6)));
    };
    for (int r = 0; r < m_cfg->objectives.size(); ++r)
        fill(r, m_cfg->objectives[r]);
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
        fill(r, o);
    });
    connect(del, &QPushButton::clicked, &dlg, [&] {
        if (table->currentRow() >= 0)
            table->removeRow(table->currentRow());
    });
    connect(def, &QPushButton::clicked, &dlg, [&] {
        const auto d = MicroscopeConfig::defaultObjectives();
        table->setRowCount(int(d.size()));
        for (int r = 0; r < d.size(); ++r)
            fill(r, d[r]);
    });
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QList<Objective> list;
    for (int r = 0; r < table->rowCount(); ++r) {
        Objective o;
        auto txt = [&](int c) { return table->item(r, c) ? table->item(r, c)->text() : QString(); };
        o.name = txt(0);
        o.magnification = std::max(0.1, txt(1).toDouble());
        o.na = txt(2).toDouble();
        o.immersion = txt(3);
        o.calibratedUmPerPixel = std::max(0.0, txt(4).toDouble());
        if (r < m_cfg->objectives.size())
            o.shadingFile = m_cfg->objectives[r].shadingFile;
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
