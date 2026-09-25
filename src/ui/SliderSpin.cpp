#include "SliderSpin.h"

#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>

#include <cmath>

namespace lm {

namespace {
constexpr int kSteps = 1000;
}

SliderSpin::SliderSpin(const QString &label, double min, double max, int decimals, QWidget *parent, bool logarithmic,
                       const QString &suffix)
    : QWidget(parent), m_min(min), m_max(max), m_log(logarithmic && min > 0)
{
    auto *g = new QGridLayout(this);
    g->setContentsMargins(0, 0, 0, 0);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(2);
    m_label = new QLabel(label, this);
    m_label->setObjectName(QStringLiteral("ControlLabel"));
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, kSteps);
    m_spin = new QDoubleSpinBox(this);
    m_spin->setRange(min, max);
    m_spin->setDecimals(decimals);
    m_spin->setKeyboardTracking(false);
    m_spin->setSuffix(suffix);
    m_spin->setMinimumWidth(86);
    m_spin->setAlignment(Qt::AlignRight);
    m_spin->setSingleStep(m_log ? std::pow(10.0, -decimals) * 10 : (max - min) / 100.0);
    if (m_log)
        m_spin->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
    g->addWidget(m_label, 0, 0, 1, 2);
    g->addWidget(m_slider, 1, 0);
    g->addWidget(m_spin, 1, 1);
    g->setColumnStretch(0, 1);

    connect(m_slider, &QSlider::valueChanged, this, [this](int s) {
        if (m_updating)
            return;
        m_value = fromSlider(s);
        m_updating = true;
        m_spin->setValue(m_value);
        m_updating = false;
        emit valueChanged(m_value);
    });
    connect(m_slider, &QSlider::sliderReleased, this, [this] { emit editingFinished(m_value); });
    connect(m_spin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_updating)
            return;
        m_value = v;
        m_updating = true;
        m_slider->setValue(toSlider(v));
        m_updating = false;
        emit valueChanged(v);
        emit editingFinished(v);
    });
}

int SliderSpin::toSlider(double v) const
{
    v = std::clamp(v, m_min, m_max);
    double t = m_log ? (std::log(v) - std::log(m_min)) / (std::log(m_max) - std::log(m_min)) : (v - m_min) / (m_max - m_min);
    return int(std::lround(t * kSteps));
}

double SliderSpin::fromSlider(int s) const
{
    const double t = double(s) / kSteps;
    double v = m_log ? std::exp(std::log(m_min) + t * (std::log(m_max) - std::log(m_min))) : m_min + t * (m_max - m_min);
    // round to the displayed precision
    const double q = std::pow(10.0, m_spin->decimals());
    if (m_log && v > 0) {
        // keep ~3 significant digits on a log scale
        const double mag = std::pow(10.0, std::floor(std::log10(v)) - 2);
        v = std::round(v / mag) * mag;
    }
    return std::round(v * q) / q;
}

void SliderSpin::setRange(double min, double max)
{
    m_min = min;
    m_max = max;
    m_log = m_log && min > 0;
    m_updating = true;
    m_spin->setRange(min, max);
    m_slider->setValue(toSlider(m_value));
    m_updating = false;
}

void SliderSpin::setDefault(double v)
{
    m_default = v;
    if (!m_reset) {
        m_reset = new QToolButton(this);
        m_reset->setObjectName(QStringLiteral("ResetButton"));
        m_reset->setText(QStringLiteral("⟲"));
        m_reset->setToolTip(tr("Reset to default (%1)").arg(v));
        m_reset->setAutoRaise(true);
        static_cast<QGridLayout *>(layout())->addWidget(m_reset, 1, 2);
        connect(m_reset, &QToolButton::clicked, this, [this] { setValueAndEmit(m_default); });
    }
}

void SliderSpin::setSingleStep(double s)
{
    m_spin->setSingleStep(s);
}

void SliderSpin::setEnabledControls(bool on)
{
    m_slider->setEnabled(on);
    m_spin->setEnabled(on);
    if (m_reset)
        m_reset->setEnabled(on);
}

void SliderSpin::setValue(double v)
{
    m_value = std::clamp(v, m_min, m_max);
    m_updating = true;
    m_spin->setValue(m_value);
    m_slider->setValue(toSlider(m_value));
    m_updating = false;
}

void SliderSpin::setValueAndEmit(double v)
{
    setValue(v);
    emit valueChanged(m_value);
    emit editingFinished(m_value);
}

} // namespace lm
