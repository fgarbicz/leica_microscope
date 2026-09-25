#pragma once
// Batch export of images (e.g. for presentations): format conversion,
// optional resizing, burned-in scale bar and annotations.

#include <QDialog>
#include <QStringList>

class QComboBox;
class QCheckBox;
class QSpinBox;
class QLineEdit;
class QProgressBar;
class QLabel;
class QPushButton;

namespace lm {

class BatchExportDialog : public QDialog {
    Q_OBJECT
public:
    BatchExportDialog(const QStringList &files, QWidget *parent = nullptr);

private:
    void run();

    QStringList m_files;
    QComboBox *m_format;
    QSpinBox *m_quality;
    QSpinBox *m_maxWidth;
    QCheckBox *m_scaleBar;
    QCheckBox *m_annotations;
    QLineEdit *m_folder;
    QProgressBar *m_progress;
    QLabel *m_status;
    QPushButton *m_start;
    bool m_cancel = false;
    bool m_running = false;
};

} // namespace lm
