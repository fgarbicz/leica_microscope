#pragma once
// Labelled slider + spin box pair for double values, optionally logarithmic
// (used for exposure time which spans six decades).

#include <QWidget>

class QSlider;
class QDoubleSpinBox;
class QLabel;
class QToolButton;

namespace lm {

class SliderSpin : public QWidget {
    Q_OBJECT
public:
    SliderSpin(const QString &label, double min, double max, int decimals, QWidget *parent = nullptr,
               bool logarithmic = false, const QString &suffix = QString());
    double value() const { return m_value; }
    void setRange(double min, double max);
    void setDefault(double v); // enables the reset button
    void setSingleStep(double s);
    void setEnabledControls(bool on);
    QDoubleSpinBox *spinBox() const { return m_spin; }

public slots:
    void setValue(double v);        // does not emit
    void setValueAndEmit(double v);

signals:
    void valueChanged(double v);    // emitted on user interaction
    void editingFinished(double v); // slider released / spin editing finished

private:
    int toSlider(double v) const;
    double fromSlider(int s) const;

    QLabel *m_label;
    QSlider *m_slider;
    QDoubleSpinBox *m_spin;
    QToolButton *m_reset = nullptr;
    double m_min, m_max, m_value = 0, m_default = 0;
    bool m_log;
    bool m_updating = false;
};

} // namespace lm
