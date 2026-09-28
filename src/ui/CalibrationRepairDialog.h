#pragma once
// Correcting the pixel size stored in images that were saved before the camera
// adapter was measured. See io/CalibrationRepair.h.

#include "io/CalibrationRepair.h"

#include <QDialog>
#include <QList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QToolButton;

namespace lm {

class CalibrationRepairDialog : public QDialog {
    Q_OBJECT
public:
    CalibrationRepairDialog(const QString &folder, double wrongAdapter, double rightAdapter,
                            QWidget *parent = nullptr);

    void reject() override; // not while files are being rewritten

private:
    void scan();
    void apply();
    void clearList(); // the folder changed: the list no longer applies
    void setBusy(bool busy);

    double m_wrongAdapter, m_rightAdapter;
    double m_sensorPixelUm;
    QLineEdit *m_folder;
    QToolButton *m_browse;
    QCheckBox *m_recursive;
    QPushButton *m_scan;
    QLabel *m_summary;
    QTableWidget *m_table;
    QProgressBar *m_progress;
    QPushButton *m_apply;
    QPushButton *m_close;
    QList<CalibrationFix> m_fixes;
    QList<CalibrationFix> m_leftAlone;
    QString m_scannedFolder; // the folder m_fixes came from
    bool m_busy = false;
};

} // namespace lm
