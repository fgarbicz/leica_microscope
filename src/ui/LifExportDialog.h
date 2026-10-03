#pragma once
// Exporting from a Leica .lif: the image shown, the images selected in the
// viewer, or all of them; as the original data (ImageJ hyperstack TIFF) or as
// pictures with the colours and contrast set in the viewer. See io/LifExport.h.

#include "io/LifExport.h"

#include <QDialog>
#include <QHash>

#include <atomic>
#include <memory>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;

namespace lm {

class LifExportDialog : public QDialog {
    Q_OBJECT
public:
    enum Scope { CurrentImage, SelectedImages, AllImages };

    LifExportDialog(const LifFileIndex &index, const LifExportItem &current, const QList<int> &selected,
                    const QHash<int, QList<LifChannelDisplay>> &displays, QWidget *parent = nullptr);
    void setScope(Scope s);

    // What the dialog would export with its current settings.
    QList<LifExportItem> items() const;
    LifExportOptions options() const;

public slots:
    void reject() override; // cancels a running export instead of closing

private:
    void updateControls();
    void run();

    const LifFileIndex &m_index;
    LifExportItem m_current;
    QList<int> m_selected;
    QHash<int, QList<LifChannelDisplay>> m_displays;

    QRadioButton *m_scope[3];
    QRadioButton *m_original;
    QRadioButton *m_pictures;
    QComboBox *m_planes;
    QComboBox *m_format;
    QCheckBox *m_scaleBar;
    QCheckBox *m_merge;
    QCheckBox *m_subfolder;
    QCheckBox *m_summary;
    QLineEdit *m_folder;
    QLabel *m_pictureLabel[2];
    QProgressBar *m_progress;
    QLabel *m_status;
    QPushButton *m_start;
    bool m_running = false;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
};

} // namespace lm
