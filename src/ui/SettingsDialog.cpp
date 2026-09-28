#include "ui/Theme.h"
#include "SettingsDialog.h"

#include "ui/Icons.h"

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
    resize(px(560), px(400));
    auto &S = AppSettings::instance();
    m_themeOnEntry = S.theme;
    m_scaleOnEntry = S.uiScale;
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_theme = new QComboBox(this);
    m_theme->addItem(tr("Dark"), QStringLiteral("dark"));
    m_theme->addItem(tr("Light"), QStringLiteral("light"));
    m_theme->setCurrentIndex(S.theme == QLatin1String("light") ? 1 : 0);
    form->addRow(tr("Appearance"), m_theme);

    // Interface size. It previews immediately, because judging a text size from
    // a number in a list does not work.
    m_uiScale = new QComboBox(this);
    const QList<QPair<int, QString>> sizes = {
        {75, tr("75%  (very small)")},   {85, tr("85%  (small)")},
        {100, tr("100%  (default)")},    {115, tr("115%  (large)")},
        {130, tr("130%  (larger)")},     {150, tr("150%  (very large)")},
        {175, tr("175%  (huge)")},       {200, tr("200%  (maximum)")},
    };
    for (const auto &[value, label] : sizes)
        m_uiScale->addItem(label, value);
    int idx = m_uiScale->findData(S.uiScale);
    if (idx < 0) { // a size set by hand, between the steps
        m_uiScale->addItem(tr("%1%").arg(S.uiScale), S.uiScale);
        idx = m_uiScale->count() - 1;
    }
    m_uiScale->setCurrentIndex(idx);
    // the same shortcuts as View > Interface size (Ctrl/Cmd is for image zoom)
    m_uiScale->setToolTip(tr("Size of the text, controls and icons of the whole application. "
                             "Also on the View menu, with %1 and %2.")
                              .arg(QKeySequence(QStringLiteral("Ctrl+Shift+=")).toString(QKeySequence::NativeText),
                                   QKeySequence(QStringLiteral("Ctrl+Shift+-")).toString(QKeySequence::NativeText)));
    form->addRow(tr("Interface size"), m_uiScale);
    m_galleryLayout = new QComboBox(this);
    m_galleryLayout->addItem(icon(Icon::LayoutReel), tr("Reel under the live image"));
    m_galleryLayout->addItem(icon(Icon::LayoutList), tr("List beside the live image, with names"));
    m_galleryLayout->addItem(icon(Icon::LayoutCompact), tr("Compact list: small thumbnails, many images"));
    m_galleryLayout->setCurrentIndex(!S.galleryVertical ? 0 : S.galleryCompact ? 2 : 1);
    m_galleryLayout->setToolTip(tr("Where the images captured in this session are shown. Also switched with the "
                                   "Reel, List and Compact buttons above them, and on the View menu."));
    form->addRow(tr("Captured images"), m_galleryLayout);
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
    // Cancel, Esc and the title bar's close button all end in reject()
    connect(bb, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(m_theme, &QComboBox::activated, this, [this] {
        emit appearanceChanged(m_theme->currentData().toString(), m_uiScale->currentData().toInt());
    });
    connect(m_uiScale, &QComboBox::activated, this, [this] {
        emit appearanceChanged(m_theme->currentData().toString(), m_uiScale->currentData().toInt());
    });
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
            // The main window does the erasing, on its way out: it first asks
            // about an unsaved result and a capture in progress, like any quit,
            // and must not write its own settings back after they are cleared.
            reject();
            emit resetAllRequested();
        }
    });
}

void SettingsDialog::reject()
{
    // put back what the previews changed (the combos show what is applied)
    if (m_theme->currentData().toString() != m_themeOnEntry || m_uiScale->currentData().toInt() != m_scaleOnEntry)
        emit appearanceChanged(m_themeOnEntry, m_scaleOnEntry);
    QDialog::reject();
}

int SettingsDialog::galleryMode() const
{
    return m_galleryLayout->currentIndex(); // in the order of MainWindow::GalleryMode
}

void SettingsDialog::accept()
{
    // An empty folder would make captures land in the working directory, which
    // is "/" for an application started from the Finder, and a relative path
    // depends on how the program was started.
    const QString folder = QDir::cleanPath(m_folder->text().trimmed());
    if (m_folder->text().trimmed().isEmpty() || QDir::isRelativePath(folder)) {
        QMessageBox::warning(this, tr("Image folder"),
                             m_folder->text().trimmed().isEmpty()
                                 ? tr("Choose a folder for the images. The default is %1.")
                                       .arg(QDir::toNativeSeparators(AppSettings::defaultFolder()))
                                 : tr("\"%1\" is not a complete folder path. Choose the folder with the … button.")
                                       .arg(m_folder->text()));
        if (m_folder->text().trimmed().isEmpty())
            m_folder->setText(QDir::toNativeSeparators(AppSettings::defaultFolder()));
        m_folder->setFocus();
        return;
    }
    auto &S = AppSettings::instance();
    S.theme = m_theme->currentData().toString();
    S.uiScale = m_uiScale->currentData().toInt();
    S.capture.operatorName = m_operator->text();
    S.capture.folder = QDir::fromNativeSeparators(folder);
    S.capture.pattern = m_pattern->text();
    S.capture.counterDigits = m_digits->value();
    S.save();
    // the previews already applied these; this makes them stick
    emit appearanceChanged(S.theme, S.uiScale);
    QDialog::accept();
}

AboutDialog::AboutDialog(const QString &cameraInfo, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("About DM Imaging"));
    auto *lay = new QVBoxLayout(this);
    auto *logo = new QLabel(this);
    logo->setPixmap(QIcon(QStringLiteral(":/icons/app.png")).pixmap(lm::iconSize(56)));
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
                               "<p>Copyright &copy; 2026 Filip Garbicz. Free software under the GNU General Public "
                               "License v3 or later, with no warranty. Uses Qt (LGPL v3) and libusb (LGPL 2.1); the "
                               "license files are in the installation folder.</p>"
                               "<p><b>Camera:</b> %1<br>"
                               "<b>System:</b> %2<br>"
                               "<b>Qt:</b> %3</p>")
                                .arg(cameraInfo.isEmpty() ? tr("not connected") : cameraInfo.toHtmlEscaped(),
                                     platformDescription().toHtmlEscaped(), QString::fromLatin1(qVersion())),
                            this);
    text->setWordWrap(true);
    text->setMinimumWidth(px(420));
    lay->addWidget(text);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

} // namespace lm
