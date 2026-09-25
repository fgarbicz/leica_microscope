#include "SettingsDialog.h"

#include "app/AppSettings.h"
#include "ui/PlatformUi.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
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
    m_pattern->setToolTip(tr("Placeholders replaced when saving: {sample} {objective} {date} {time} {counter} {mode} "
                             "{operator}. Example: {sample}_{objective}_{counter} gives Liver01_20x_003."));
    form->addRow(tr("File name template"), m_pattern);
    m_digits = new QSpinBox(this);
    m_digits->setRange(1, 8);
    m_digits->setValue(S.capture.counterDigits);
    form->addRow(tr("Counter digits"), m_digits);
    lay->addLayout(form);

    auto *reset = new QPushButton(tr("Reset all settings…"), this);
    auto *row = new QHBoxLayout;
    // no camera driver to install on macOS: the button would do nothing useful
    QPushButton *drv = nullptr;
    if (cameraAccessSetupAvailable()) {
        drv = new QPushButton(cameraAccessSetupLabel().remove(QLatin1Char('&')), this);
        row->addWidget(drv);
    }
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
    if (drv)
        connect(drv, &QPushButton::clicked, this, [this] { setUpCameraAccess(this); });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (QMessageBox::warning(this, tr("Reset all settings"),
                                 tr("This erases ALL settings, including the objective calibrations (µm/pixel), shading "
                                    "references, exposure and white balance saved per objective, and colour presets.\n\n"
                                    "This cannot be undone. The application will close."),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
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
    auto *logo = new QLabel(this);
    logo->setPixmap(QIcon(QStringLiteral(":/icons/app.png")).pixmap(56, 56));
    auto *head = new QHBoxLayout;
    head->addWidget(logo);
    auto *title = new QLabel(QStringLiteral("<h2>DM Imaging %1</h2><p>build " DMI_GIT_HASH "</p>")
                                 .arg(QCoreApplication::applicationVersion()),
                             this);
    head->addWidget(title, 1);
    lay->addLayout(head);
    auto *text = new QLabel(tr("<p>Image acquisition and analysis for the Leica DM2000 microscope.</p>"
                               "<p>Native driver for the Leica DMC6200 camera (Jenoptik GRYPHAX platform, "
                               "Sony IMX174 sensor): live imaging, pixel-shift capture, extended depth of field, "
                               "live stitching, calibrated measurements and annotation.</p>"
                               "<p><b>Camera:</b> %1<br>"
                               "<b>System:</b> %2<br>"
                               "<b>Qt:</b> %3</p>")
                                .arg(cameraInfo.isEmpty() ? tr("not connected") : cameraInfo.toHtmlEscaped(),
                                     platformDescription().toHtmlEscaped(), QString::fromLatin1(qVersion())),
                            this);
    text->setWordWrap(true);
    text->setMinimumWidth(420);
    lay->addWidget(text);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

} // namespace lm
