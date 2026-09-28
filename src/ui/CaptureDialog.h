#pragma once
// Shown after every manual capture: asks for the objective magnification
// (the DM2000 turret is not coded, so the software cannot detect it) and the
// image name before the image is saved.

#include <QDialog>
#include <QImage>

#include <functional>

class QButtonGroup;
class QLineEdit;
class QLabel;

namespace lm {

class MicroscopeConfig;

class CaptureDialog : public QDialog {
    Q_OBJECT
public:
    // nameForObjective: suggested file name (without extension) for an objective index
    CaptureDialog(const QImage &preview, const MicroscopeConfig &scope, int objective,
                  std::function<QString(int)> nameForObjective, const QString &info, QWidget *parent = nullptr);

    int objectiveIndex() const;
    QString imageName() const;
    QString notes() const;

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void reject() override;

private:
    void onObjectiveChanged(int index);

    QButtonGroup *m_group;
    QLineEdit *m_name;
    QLineEdit *m_notes;
    std::function<QString(int)> m_nameFor;
    QString m_autoName; // last suggested name (replaced while the user has not edited it)
};

} // namespace lm
