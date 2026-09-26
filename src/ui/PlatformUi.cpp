#include "PlatformUi.h"

#include "camera/usb/UsbDevice.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#ifdef Q_OS_LINUX
// Qt6::DBus is linked on Linux only (see CMakeLists.txt).
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#endif

namespace lm {

namespace {

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
// Only the platforms that have a camera-access step below need these helpers.

// Looks for `relative` next to the executable, then in the directories above
// it: the installed layout has <app>/driver/..., a development build has
// <repo>/driver/... several levels up from build/bin.
QString findResource(const QString &relative)
{
    QDir d(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        const QString p = d.filePath(relative);
        if (QFileInfo::exists(p))
            return p;
        if (!d.cdUp())
            break;
    }
#ifdef Q_OS_MACOS
    // inside a bundle the resources live in Contents/Resources
    QDir r(QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources"));
    const QString p = r.filePath(relative);
    if (QFileInfo::exists(p))
        return p;
#endif
    return {};
}

// Shows the output of a helper process in an expandable "Details" box.
QString reportResult(QWidget *parent, bool ok, const QString &title, const QString &okText, const QString &failText,
                     const QString &details)
{
    QMessageBox box(ok ? QMessageBox::Information : QMessageBox::Warning, title, ok ? okText : failText,
                    QMessageBox::Ok, parent);
    if (!details.trimmed().isEmpty())
        box.setDetailedText(details);
    box.exec();
    return ok ? QObject::tr("Camera access configured") : QObject::tr("Camera setup failed");
}
#endif // Q_OS_WIN || Q_OS_LINUX

} // namespace

void revealInFileManager(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
        return;
    }
#ifdef Q_OS_WIN
    if (QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("/select,") + QDir::toNativeSeparators(fi.absoluteFilePath())}))
        return;
#elif defined(Q_OS_MACOS)
    if (QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), fi.absoluteFilePath()}))
        return;
#elif defined(Q_OS_LINUX)
    // The freedesktop file manager interface selects the file; not every
    // desktop implements it, hence the fallback below.
    QDBusInterface fm(QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
                      QStringLiteral("org.freedesktop.FileManager1"), QDBusConnection::sessionBus());
    if (fm.isValid()) {
        const QDBusReply<void> reply =
            fm.call(QStringLiteral("ShowItems"), QStringList{QUrl::fromLocalFile(fi.absoluteFilePath()).toString()},
                    QString());
        if (reply.isValid())
            return;
    }
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

bool openInImageViewer(const QString &path, QWidget *parent)
{
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(parent, QObject::tr("Open image"),
                             QObject::tr("%1 no longer exists (moved or deleted?).").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    // ShellExecute on Windows, LaunchServices ("open") on macOS, xdg-open on Linux
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        return true;
    QMessageBox::warning(parent, QObject::tr("Open image"),
                         QObject::tr("No program is set up to open %1 files. Choose a default image viewer for "
                                     "TIFF files in the system settings, or use %2.")
                             .arg(QFileInfo(path).suffix().toUpper(), revealActionText()));
    return false;
}

bool cameraAccessSetupAvailable()
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    return true;
#else
    return false; // macOS opens a vendor-class device without a driver
#endif
}

QString cameraAccessSetupLabel()
{
#ifdef Q_OS_WIN
    return QObject::tr("Install / repair camera &driver…");
#elif defined(Q_OS_LINUX)
    return QObject::tr("Install camera &access rule (udev)…");
#else
    return QObject::tr("Camera &access…");
#endif
}

QString setUpCameraAccess(QWidget *parent)
{
#ifdef Q_OS_WIN
    const QString script = findResource(QStringLiteral("driver/install_driver.ps1"));
    if (script.isEmpty()) {
        QMessageBox::warning(parent, QObject::tr("Camera driver"),
                             QObject::tr("driver/install_driver.ps1 was not found."));
        return QObject::tr("Driver installer not found");
    }
    if (QMessageBox::question(parent, QObject::tr("Camera driver"),
                              QObject::tr("Install (or repair) the USB driver for the Leica DMC6200 camera?\n\n"
                                          "Windows will ask for administrator permission."))
        != QMessageBox::Yes)
        return {};
    QProcess p;
    p.start(QStringLiteral("powershell.exe"),
            {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
             QStringLiteral("-File"), QDir::toNativeSeparators(script)});
    QApplication::setOverrideCursor(Qt::WaitCursor);
    p.waitForFinished(180000);
    QApplication::restoreOverrideCursor();
    const QString out =
        QString::fromLocal8Bit(p.readAllStandardOutput()) + QString::fromLocal8Bit(p.readAllStandardError());
    const bool ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    return reportResult(parent, ok, QObject::tr("Camera driver"), QObject::tr("The camera driver was installed."),
                        QObject::tr("Driver installation failed. Check that you allowed the administrator request, "
                                    "then try again. The details below say what went wrong."),
                        out);

#elif defined(Q_OS_LINUX)
    const QString rule = findResource(QStringLiteral("driver/99-leica-dmc6200.rules"));
    if (rule.isEmpty()) {
        QMessageBox::warning(parent, QObject::tr("Camera access"),
                             QObject::tr("driver/99-leica-dmc6200.rules was not found."));
        return QObject::tr("udev rule not found");
    }
    if (QMessageBox::question(parent, QObject::tr("Camera access"),
                              QObject::tr("Install the udev rule that lets DM Imaging use the Leica DMC6200 without "
                                          "root?\n\nThe rule is copied to /etc/udev/rules.d/ and you will be asked for "
                                          "your password. Unplug and replug the camera afterwards."))
        != QMessageBox::Yes)
        return {};
    // pkexec gives a graphical password prompt; without it the user has to run
    // driver/install_udev_rule.sh from a terminal.
    const QString helper = findResource(QStringLiteral("driver/install_udev_rule.sh"));
    QProcess p;
    if (!helper.isEmpty() && !QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
        p.start(QStringLiteral("pkexec"), {QStringLiteral("/bin/sh"), helper});
    } else if (!QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
        p.start(QStringLiteral("pkexec"),
                {QStringLiteral("install"), QStringLiteral("-m"), QStringLiteral("0644"), rule,
                 QStringLiteral("/etc/udev/rules.d/99-leica-dmc6200.rules")});
    } else {
        QMessageBox::information(parent, QObject::tr("Camera access"),
                                 QObject::tr("pkexec is not installed, so the rule cannot be installed from here.\n\n"
                                             "Run this in a terminal instead:\n\n    sudo sh %1")
                                     .arg(helper.isEmpty() ? rule : helper));
        return QObject::tr("Install the udev rule manually");
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    p.waitForFinished(180000);
    QApplication::restoreOverrideCursor();
    const QString out =
        QString::fromLocal8Bit(p.readAllStandardOutput()) + QString::fromLocal8Bit(p.readAllStandardError());
    const bool ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    return reportResult(parent, ok, QObject::tr("Camera access"),
                        QObject::tr("The access rule was installed. Unplug the camera and plug it back in."),
                        QObject::tr("The rule could not be installed. The details below say what went wrong; you can "
                                    "also run driver/install_udev_rule.sh from a terminal."),
                        out);

#else
    QMessageBox::information(parent, QObject::tr("Camera access"),
                             QObject::tr("macOS needs no driver for the Leica camera: DM Imaging talks to it "
                                         "directly over USB.\n\nIf the camera is not found, unplug it for five "
                                         "seconds and plug it back in, and make sure no other imaging application "
                                         "has it open."));
    return {};
#endif
}

QString revealActionText()
{
#ifdef Q_OS_WIN
    return QObject::tr("Show in Explorer");
#elif defined(Q_OS_MACOS)
    return QObject::tr("Show in Finder");
#else
    return QObject::tr("Show in file manager");
#endif
}

QString trashName()
{
#ifdef Q_OS_WIN
    return QObject::tr("recycle bin");
#else
    return QObject::tr("Trash");
#endif
}

QString platformDescription()
{
    return QStringLiteral("%1 %2 · %3")
        .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(),
             QString::fromStdString(usb::Device::backendDescription()));
}

} // namespace lm
