#include "SettingsDialog.h"

#include "app/AppSettings.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace lm {

static QString findDriverScript()
{
    // installed layout: <app>/driver/install_driver.ps1, development: <repo>/driver/...
    QDir d(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 5; ++i) {
        const QString p = d.filePath(QStringLiteral("driver/install_driver.ps1"));
        if (QFileInfo::exists(p))
            return p;
        if (!d.cdUp())
            break;
    }
    return {};
}

QString installCameraDriver(QWidget *parent)
{
    const QString script = findDriverScript();
    if (script.isEmpty()) {
        QMessageBox::warning(parent, QObject::tr("Camera driver"), QObject::tr("driver/install_driver.ps1 was not found."));
        return QObject::tr("Driver installer not found");
    }
    if (QMessageBox::question(parent, QObject::tr("Camera driver"),
                              QObject::tr("Install (or repair) the USB driver for the Leica DMC6200 camera?\n\n"
                                          "Windows will ask for administrator permission.")) != QMessageBox::Yes)
        return {};
    QProcess p;
    p.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                                               QStringLiteral("Bypass"), QStringLiteral("-File"), QDir::toNativeSeparators(script)});
    QApplication::setOverrideCursor(Qt::WaitCursor);
    p.waitForFinished(180000);
    QApplication::restoreOverrideCursor();
    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput()) + QString::fromLocal8Bit(p.readAllStandardError());
    const bool ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    QMessageBox box(ok ? QMessageBox::Information : QMessageBox::Warning, QObject::tr("Camera driver"),
                    ok ? QObject::tr("The camera driver was installed.") : QObject::tr("Driver installation failed."),
                    QMessageBox::Ok, parent);
    box.setDetailedText(out);
    box.exec();
    return ok ? QObject::tr("Camera driver installed") : QObject::tr("Driver installation failed");
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Settings"));
    resize(560, 360);
    auto &S = AppSettings::instance();
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_theme = new QComboBox(this);
    m_theme->addItem(tr("Dark"), QStringLiteral("dark"));
    m_theme->addItem(tr("Light"), QStringLiteral("light"));
    m_theme->setCurrentIndex(S.theme == QLatin1String("light") ? 1 : 0);
    form->addRow(tr("Appearance"), m_theme);
    m_operator = new QLineEdit(S.capture.operatorName, this);
    form->addRow(tr("Operator"), m_operator);
    auto *fr = new QHBoxLayout;
    m_folder = new QLineEdit(S.capture.folder, this);
    auto *browse = new QToolButton(this);
    browse->setText(QStringLiteral("…"));
    fr->addWidget(m_folder, 1);
    fr->addWidget(browse);
    form->addRow(tr("Image folder"), fr);
    m_pattern = new QLineEdit(S.capture.pattern, this);
    m_pattern->setToolTip(tr("Tokens: {sample} {objective} {date} {time} {counter} {mode} {operator}"));
    form->addRow(tr("File name template"), m_pattern);
    m_digits = new QSpinBox(this);
    m_digits->setRange(1, 8);
    m_digits->setValue(S.capture.counterDigits);
    form->addRow(tr("Counter digits"), m_digits);
    lay->addLayout(form);

    auto *drv = new QPushButton(tr("Install / repair camera driver…"), this);
    auto *reset = new QPushButton(tr("Reset all settings…"), this);
    auto *row = new QHBoxLayout;
    row->addWidget(drv);
    row->addWidget(reset);
    row->addStretch();
    lay->addLayout(row);
    lay->addStretch();
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Image folder"), m_folder->text());
        if (!d.isEmpty())
            m_folder->setText(d);
    });
    connect(drv, &QPushButton::clicked, this, [this] { installCameraDriver(this); });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Reset"), tr("Reset all settings to their defaults? The application will close."))
            == QMessageBox::Yes) {
            QSettings().clear();
            QSettings().sync();
            qApp->exit(0);
        }
    });
}

void SettingsDialog::accept()
{
    auto &S = AppSettings::instance();
    const QString theme = m_theme->currentData().toString();
    const bool themeChangedFlag = theme != S.theme;
    S.theme = theme;
    S.capture.operatorName = m_operator->text();
    S.capture.folder = m_folder->text();
    S.capture.pattern = m_pattern->text();
    S.capture.counterDigits = m_digits->value();
    S.save();
    if (themeChangedFlag)
        emit themeChanged(theme);
    QDialog::accept();
}

AboutDialog::AboutDialog(const QString &cameraInfo, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("About DM Imaging"));
    auto *lay = new QVBoxLayout(this);
    auto *title = new QLabel(QStringLiteral("<h2>DM Imaging %1</h2>").arg(QCoreApplication::applicationVersion()), this);
    lay->addWidget(title);
    auto *text = new QLabel(tr("<p>Image acquisition and analysis for the Leica DM2000 microscope.</p>"
                               "<p>Native driver for the Leica DMC6200 camera (Jenoptik GRYPHAX platform, "
                               "Sony IMX174 sensor): live imaging, pixel-shift capture, extended depth of field, "
                               "live stitching, calibrated measurements and annotation.</p>"
                               "<p><b>Camera:</b> %1</p>"
                               "<p>Built with Qt %2.</p>")
                                .arg(cameraInfo.isEmpty() ? tr("not connected") : cameraInfo.toHtmlEscaped(), QString::fromLatin1(qVersion())),
                            this);
    text->setWordWrap(true);
    text->setMinimumWidth(420);
    lay->addWidget(text);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

} // namespace lm
