#pragma once
// Process workspace: viewing, annotation, measurement, adjustment and export
// of saved images, plus offline extended depth of field and stitching.

#include "imaging/ColorPipeline.h"
#include "io/ImageIO.h"

#include <QImage>
#include <QTimer>

#include <QWidget>

class QTableWidget;
class QLabel;
class QActionGroup;
class QToolButton;
class QDoubleSpinBox;
class QCheckBox;

class QToolBar;

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
    ImageView *imageView() const { return m_view; }
    // Work that exists nowhere else: a result built here (multifocus / stitching
    // from files) that has no file yet, or annotations or a pixel size added to an
    // image with no file of its own (one opened from a .lif - its pixels are safe
    // in the .lif, but the additions have nowhere to be saved).
    bool hasUnsavedResult() const { return !m_data.empty() && (m_unsavedResult || m_unsavedEdits); }
    // asks Save / Discard / Cancel for an unsaved result; true = go ahead
    bool maybeDiscardUnsaved();
    // writes annotation changes still waiting for the save timer
    void flushAnnotations();

public slots:
    void openDialog();
    void saveAs();
    void exportWithOverlays();
    void copyToClipboard();
    void print();
    void multifocusFromFiles();
    void stitchFromFiles();
    void setCalibration();
    void analyzeIhc(bool regionOnly, bool nuclei = false);
    void analyzeIhcImpl(bool regionOnly, bool nuclei);
    void updateStainLabel();

signals:
    void message(const QString &text, int timeoutMs);
    void fileSaved(const QString &path);

protected:
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    QToolBar *m_toolbar = nullptr;
    void rerender();
    void updateMeasurements();
    void updateInfo();
    void saveAnnotations();
    void updateColorButton();

    ImageView *m_view;
    AnnotationLayer *m_layer = nullptr;
    QTableWidget *m_table;
    QLabel *m_info;
    QActionGroup *m_tools;
    QToolButton *m_colorBtn = nullptr;
    SliderSpin *m_brightness, *m_contrast, *m_gamma, *m_saturation, *m_sharpen;
    Image16 m_data;
    ImageMetadata m_meta;
    QString m_path;
    ColorSettings m_adjust;
    bool m_dirtyAnnotations = false;
    bool m_unsavedResult = false;
    bool m_unsavedEdits = false; // see hasUnsavedResult()
    QTimer m_annotationSave; // debounces writing the annotation sidecar
    // IHC quantification
    SliderSpin *m_dabThreshold;
    SliderSpin *m_nucleusDiameter;
    QLabel *m_ihcResult;
    QCheckBox *m_ihcOverlay;
    QLabel *m_stainLabel;
    QImage m_ihcMask;
    QString m_ihcText;
};

} // namespace lm
