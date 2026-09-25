#pragma once
// Right side panel of the Acquire workspace: histogram, overlays and
// image information.

#include "app/AcquisitionEngine.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QDoubleSpinBox;
class QSpinBox;

namespace lm {

class HistogramWidget;

class ToolsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ToolsPanel(QWidget *parent = nullptr);

    HistogramWidget *histogram() const { return m_hist; }
    void setStats(const LiveStats &s, double umPerPixel);
    void setPixelInfo(const QString &text);
    void setLevels(double black, double white);
    void resetFocusPeak() { m_focusPeak = 0; }
    double focusPeak() const { return m_focusPeak; }

signals:
    void overlaysChanged();
    void levelsChanged(double black, double white);
    void focusRegionRequested();

private:
    HistogramWidget *m_hist;
    QLabel *m_info;
    QLabel *m_pixel;
    QLabel *m_focus;
    double m_focusPeak = 0;
};

} // namespace lm
