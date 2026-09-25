#pragma once
// Batch IHC quantification: DAB colour deconvolution of many images at once,
// one table row per image, exported as CSV for statistics.

#include <QDialog>
#include <QByteArray>
#include <QStringList>

#include <vector>

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QProgressBar;
class QLabel;
class QPushButton;
class QTableWidget;

namespace lm {

class BatchIhcDialog : public QDialog {
    Q_OBJECT
public:
    BatchIhcDialog(const QStringList &files, QWidget *parent = nullptr);

    struct Row {
        QString file, objective, region, error;
        QString warning; // result valid but to be checked
        double umPerPixel = 0;
        QString colorCorrection; // from the image metadata (empty: not recorded)
        QString lightFilter;     // from the image metadata (empty: none)
        double tissueArea = 0, positiveArea = 0; // µm² when calibrated, else pixels
        double positivePct = 0, weakPct = 0, moderatePct = 0, strongPct = 0;
        double hScore = 0, meanDabPositive = 0;
        int cells = -1, positiveCells = 0;  // -1 = not counted
        double positiveCellPct = 0, cellDensity = 0;
        // image and overlay for the PDF report, JPEG compressed (~100 KB instead of
        // ~2 MB each, so large batches stay small in memory)
        QByteArray thumbJpeg, overlayJpeg;
    };

private:
    void run();
    void addRow(const Row &r);
    QString csv(QChar sep = QLatin1Char(',')) const;
    void reject() override;
    void exportCsv();
    void exportPdf();
    QString stainDescription() const;
    QString colourMixWarning() const; // empty unless corrected and uncorrected images are mixed

    QStringList m_files;
    QDoubleSpinBox *m_threshold;
    QCheckBox *m_useRegions;
    QCheckBox *m_countCells;
    QCheckBox *m_saveOverlays;
    QLineEdit *m_overlayFolder;
    QTableWidget *m_table;
    QProgressBar *m_progress;
    QLabel *m_status;
    QPushButton *m_start;
    QPushButton *m_export;
    QPushButton *m_copy;
    QPushButton *m_pdf;
    std::vector<Row> m_rows;
    bool m_cancel = false;
    bool m_running = false;
    bool m_exported = false; // results saved / copied since the last run
    // settings of the last run (exports report these, not the current widget values)
    double m_runThreshold = 0.15;
    bool m_runUseRegions = true;
};

} // namespace lm
