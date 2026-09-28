#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;

namespace lm {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    // the captured images' layout chosen here (MainWindow::GalleryMode; applied
    // by the caller on OK)
    int galleryMode() const;
    void accept() override;
    // also on Esc and the window's close button, not only Cancel
    void reject() override;

signals:
    // "Reset all settings" was confirmed. The dialog has closed; the receiver
    // quits the application (through its normal close path) and erases the
    // settings on the way out.
    void resetAllRequested();

    // Emitted while the dialog is open so the change can be seen, and again on
    // OK (or with the original values on Cancel).
    void appearanceChanged(const QString &theme, int uiScalePercent);

private:
    QComboBox *m_theme;
    QComboBox *m_uiScale;
    QComboBox *m_galleryLayout;
    QString m_themeOnEntry;
    int m_scaleOnEntry = 100;
    QLineEdit *m_operator;
    QLineEdit *m_folder;
    QLineEdit *m_pattern;
    QSpinBox *m_digits;
};

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(const QString &cameraInfo, QWidget *parent = nullptr);
};

} // namespace lm
