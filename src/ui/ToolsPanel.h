#pragma once
// Right side panel of the Acquire workspace. It builds two groups: "Image"
// (histogram, focus, information — what the picture currently is) and
// "Overlays" (what is drawn on top of it). The overlays group lives in its own
// widget so MainWindow can put the colour panel between the two, keeping the
// most used controls near the top.

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
    // The "Overlays" group, to be placed separately in the panel column.
    QWidget *overlaysPanel() const { return m_overlays; }

signals:
    void overlaysChanged();
    void levelsChanged(double black, double white);
    void focusRegionRequested();

private:
    HistogramWidget *m_hist;
    QLabel *m_info;
    QLabel *m_pixel;
    QLabel *m_focus;
    QWidget *m_overlays = nullptr;
    double m_focusPeak = 0;
};

} // namespace lm
