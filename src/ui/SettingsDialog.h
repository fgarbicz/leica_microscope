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
    void accept() override;

signals:
    void themeChanged(const QString &name);

private:
    QComboBox *m_theme;
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

// Runs driver/install_driver.ps1 elevated; returns a short status message.
QString installCameraDriver(QWidget *parent);

} // namespace lm
