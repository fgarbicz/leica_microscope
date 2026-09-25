#pragma once
// Batch IHC quantification: DAB colour deconvolution of many images at once,
// one table row per image, exported as CSV for statistics.

#include <QDialog>
#include <QImage>
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
        double umPerPixel = 0;
        double tissueArea = 0, positiveArea = 0; // µm² when calibrated, else pixels
        double positivePct = 0, weakPct = 0, moderatePct = 0, strongPct = 0;
        double hScore = 0, meanDabPositive = 0;
        int cells = -1, positiveCells = 0;  // -1 = not counted
        double positiveCellPct = 0, cellDensity = 0;
        QImage thumb, overlayThumb; // for the PDF report
    };

private:
    void run();
    void addRow(const Row &r);
    QString csv() const;
    void exportCsv();
    void exportPdf();
    QString stainDescription() const;

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
};

} // namespace lm
