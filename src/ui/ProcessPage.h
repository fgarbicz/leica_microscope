#pragma once
// Process workspace: viewing, annotation, measurement, adjustment and export
// of saved images, plus offline extended depth of field and stitching.

#include "imaging/ColorPipeline.h"
#include "io/ImageIO.h"

#include <QImage>

#include <QWidget>

class QTableWidget;
class QLabel;
class QActionGroup;
class QToolButton;
class QDoubleSpinBox;
class QCheckBox;

namespace lm {

class ImageView;
class AnnotationLayer;
class SliderSpin;

class ProcessPage : public QWidget {
    Q_OBJECT
public:
    explicit ProcessPage(QWidget *parent = nullptr);

    bool openFile(const QString &path);
    void openImage(const Image16 &img, const ImageMetadata &meta, const QString &path);
    QString currentPath() const { return m_path; }
    bool hasImage() const { return !m_data.empty(); }

public slots:
    void openDialog();
    void saveAs();
    void exportWithOverlays();
    void copyToClipboard();
    void print();
    void multifocusFromFiles();
    void stitchFromFiles();
    void setCalibration();
    void analyzeIhc(bool regionOnly);
    void updateStainLabel();

signals:
    void message(const QString &text, int timeoutMs);
    void fileSaved(const QString &path);

private:
    void rerender();
    void updateMeasurements();
    void updateInfo();
    void saveAnnotations();

    ImageView *m_view;
    AnnotationLayer *m_layer;
    QTableWidget *m_table;
    QLabel *m_info;
    QActionGroup *m_tools;
    QToolButton *m_colorBtn;
    SliderSpin *m_brightness, *m_contrast, *m_gamma, *m_saturation, *m_sharpen;
    Image16 m_data;
    ImageMetadata m_meta;
    QString m_path;
    ColorSettings m_adjust;
    bool m_dirtyAnnotations = false;
    // IHC quantification
    SliderSpin *m_dabThreshold;
    QLabel *m_ihcResult;
    QCheckBox *m_ihcOverlay;
    QLabel *m_stainLabel;
    QImage m_ihcMask;
    QString m_ihcText;
};

} // namespace lm
