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
    // Emitted while the dialog is open so the change can be seen, and again on
    // OK (or with the original values on Cancel).
    void appearanceChanged(const QString &theme, int uiScalePercent);

private:
    QComboBox *m_theme;
    QComboBox *m_uiScale;
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
