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

namespace lm {

class CalibrationRepairDialog : public QDialog {
    Q_OBJECT
public:
    CalibrationRepairDialog(const QString &folder, double wrongAdapter, double rightAdapter,
                            QWidget *parent = nullptr);

private:
    void scan();
    void apply();

    double m_wrongAdapter, m_rightAdapter;
    QLineEdit *m_folder;
    QCheckBox *m_recursive;
    QLabel *m_summary;
    QTableWidget *m_table;
    QProgressBar *m_progress;
    QPushButton *m_apply;
    QList<CalibrationFix> m_fixes;
};

} // namespace lm
