#include "PlatformUi.h"

#include "camera/usb/UsbDevice.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
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
// it: the Windows install has <app>/driver/..., a development build has
// <repo>/driver/... several levels up from build/bin. An installed Linux build
// has its files in <prefix>/share/dmimaging/ beside <prefix>/bin (see
// CMakeLists.txt), or wherever XDG_DATA_DIRS points.
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
#ifdef Q_OS_LINUX
    const QString shared = QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("../share/dmimaging/") + relative);
    if (QFileInfo::exists(shared))
        return QDir::cleanPath(shared);
    const QString located =
        QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("dmimaging/") + relative);
    if (!located.isEmpty())
        return located;
#endif
    return {};
}

// Shows the output of a helper process in an expandable "Details" box.
QString reportResult(QWidget *parent, bool ok, const QString &title, const QString &okText, const QString &failText,
                     const QString &details)
{
    QMessageBox box(ok ? QMessageBox::Information : QMessageBox::Warning, title, ok ? okText : failText,
                    QMessageBox::Ok, parent);
    box.setWindowModality(Qt::WindowModal);
    if (!details.trimmed().isEmpty())
        box.setDetailedText(details);
    box.exec();
    return ok ? QObject::tr("Camera access configured") : QObject::tr("Camera setup failed");
}

// Runs an installer (elevated by pkexec or PowerShell) without blocking the
// interface. It can take minutes - the user answers a password or UAC prompt -
// and must never be killed half-way, so there is no timeout and no cancel: a
// busy dialog stays up until the process ends by itself. The process has no
// parent, so closing the window that started it does not kill it either.
void runInstaller(QWidget *parent, const QString &program, const QStringList &args, const QString &title,
                  const QString &okText, const QString &failText, std::function<void(const QString &)> done)
{
    auto *proc = new QProcess;
    proc->setProcessChannelMode(QProcess::MergedChannels);
    auto *busy = new QProgressDialog(QObject::tr("Installing… Answer the permission request if one appears."),
                                     QString(), 0, 0, parent);
    busy->setWindowTitle(title);
    busy->setWindowModality(Qt::WindowModal);
    busy->setCancelButton(nullptr);
    busy->setMinimumDuration(0);
    busy->setAttribute(Qt::WA_DeleteOnClose);
    busy->show();
    QPointer<QWidget> owner = parent;
    QPointer<QProgressDialog> dialog = busy;
    auto finish = [=](bool ok, const QString &extra) {
        if (dialog)
            dialog->close();
        const QString out = QString::fromLocal8Bit(proc->readAll()) + extra;
        proc->deleteLater();
        const QString status = reportResult(owner, ok, title, okText, failText, out);
        if (done)
            done(status);
    };
    QObject::connect(proc, &QProcess::finished, proc, [=](int code, QProcess::ExitStatus st) {
        finish(st == QProcess::NormalExit && code == 0, QString());
    });
    QObject::connect(proc, &QProcess::errorOccurred, proc, [=](QProcess::ProcessError e) {
        // the other errors end in finished() as well
        if (e == QProcess::FailedToStart)
            finish(false, QObject::tr("%1 could not be started: %2").arg(program, proc->errorString()));
    });
    proc->start(program, args);
}

#endif // Q_OS_WIN || Q_OS_LINUX

} // namespace

namespace {
// The extensions in one filter, "TIFF 16-bit (*.tif *.tiff)" -> {"tif", "tiff"}.
// "*" and "*.*" (all files) give none.
QStringList filterExtensions(const QString &filter)
{
    static const QRegularExpression pattern(QStringLiteral("\\*\\.([A-Za-z0-9_]+)"));
    QStringList exts;
    for (auto it = pattern.globalMatch(filter); it.hasNext();)
        exts << it.next().captured(1);
    return exts;
}
} // namespace

QString getSaveFileName(QWidget *parent, const QString &caption, const QString &dir, const QString &filter,
                        QString *selectedFilter)
{
    const QStringList filters = filter.split(QStringLiteral(";;"), Qt::SkipEmptyParts);
    QStringList known;
    for (const QString &f : filters)
        known << filterExtensions(f);
    QString start = dir;
    QString chosenFilter = selectedFilter ? *selectedFilter : QString();
    for (;;) {
        QString path = QFileDialog::getSaveFileName(parent, caption, start, filter, &chosenFilter);
        if (selectedFilter)
            *selectedFilter = chosenFilter;
        if (path.isEmpty())
            return {};
        if (known.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive))
            return path;
        const QStringList exts = filterExtensions(chosenFilter.isEmpty() ? filters.value(0) : chosenFilter);
        if (exts.isEmpty())
            return path; // "All files": the name is what the user typed
        path += QLatin1Char('.') + exts.first();
        // the dialog confirmed overwriting the name it returned, not this one
        if (!QFileInfo::exists(path)
            || QMessageBox::question(parent, caption,
                                     QObject::tr("%1 already exists. Replace it?").arg(QFileInfo(path).fileName()),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                   == QMessageBox::Yes)
            return path;
        start = path; // ask again, starting from the name with its extension
    }
}

void revealInFileManager(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
        return;
    }
#ifdef Q_OS_WIN
    // Explorer parses its own command line: "/select," and the quoted path must
    // reach it exactly like this. Passed as an argument list, Qt quotes the
    // whole "/select,C:\a b\c.tif" when the path has a space, and Explorer then
    // opens Documents instead.
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(
        QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(fi.absoluteFilePath())));
    if (explorer.startDetached())
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
    auto noViewer = [&] {
        QMessageBox::warning(parent, QObject::tr("Open image"),
                             QObject::tr("No program is set up to open %1 files. Choose a default image viewer for "
                                         "them in the system settings, or use %2.")
                                 .arg(QFileInfo(path).suffix().toUpper(), revealActionText()));
        return false;
    };
#ifdef Q_OS_LINUX
    // xdg-open reports success as soon as it starts, even when nothing can open
    // the file; ask for the default application first (skipped without xdg-mime),
    // once per file type and session
    static QHash<QString, bool> hasViewer;
    const QString mime = QMimeDatabase().mimeTypeForFile(path).name();
    if (!hasViewer.contains(mime)) {
        QProcess query;
        query.start(QStringLiteral("xdg-mime"), {QStringLiteral("query"), QStringLiteral("default"), mime});
        hasViewer[mime] = !(query.waitForFinished(2000) && query.exitStatus() == QProcess::NormalExit
                            && query.exitCode() == 0 && query.readAllStandardOutput().trimmed().isEmpty());
    }
    if (!hasViewer[mime])
        return noViewer();
#endif
    // ShellExecute on Windows, LaunchServices ("open") on macOS, xdg-open on Linux
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        return true;
    return noViewer();
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

void setUpCameraAccess(QWidget *parent, std::function<void(const QString &)> done)
{
    auto finished = [&done](const QString &status) {
        if (done)
            done(status);
    };
#ifdef Q_OS_WIN
    const QString script = findResource(QStringLiteral("driver/install_driver.ps1"));
    if (script.isEmpty()) {
        QMessageBox::warning(parent, QObject::tr("Camera driver"),
                             QObject::tr("driver/install_driver.ps1 was not found."));
        finished(QObject::tr("Driver installer not found"));
        return;
    }
    if (QMessageBox::question(parent, QObject::tr("Camera driver"),
                              QObject::tr("Install (or repair) the USB driver for the Leica DMC6200 camera?\n\n"
                                          "Windows will ask for administrator permission."))
        != QMessageBox::Yes) {
        finished(QString());
        return;
    }
    runInstaller(parent, QStringLiteral("powershell.exe"),
                 {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                  QStringLiteral("-File"), QDir::toNativeSeparators(script)},
                 QObject::tr("Camera driver"), QObject::tr("The camera driver was installed."),
                 QObject::tr("Driver installation failed. Check that you allowed the administrator request, "
                             "then try again. The details below say what went wrong."),
                 std::move(done));

#elif defined(Q_OS_LINUX)
    const QString rule = findResource(QStringLiteral("driver/99-leica-dmc6200.rules"));
    if (rule.isEmpty()) {
        QMessageBox::warning(parent, QObject::tr("Camera access"),
                             QObject::tr("driver/99-leica-dmc6200.rules was not found."));
        finished(QObject::tr("udev rule not found"));
        return;
    }
    if (QMessageBox::question(parent, QObject::tr("Camera access"),
                              QObject::tr("Install the udev rule that lets DM Imaging use the Leica DMC6200 without "
                                          "root?\n\nThe rule is copied to /etc/udev/rules.d/ and you will be asked for "
                                          "your password. Unplug and replug the camera afterwards."))
        != QMessageBox::Yes) {
        finished(QString());
        return;
    }
    // pkexec gives a graphical password prompt; without it the user has to run
    // driver/install_udev_rule.sh from a terminal. The helper is a bash script
    // (BASH_SOURCE, pipefail): /bin/sh is dash on Debian and cannot run it.
    const QString helper = findResource(QStringLiteral("driver/install_udev_rule.sh"));
    const bool havePkexec = !QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty();
    QString program;
    QStringList args;
    if (!helper.isEmpty() && havePkexec) {
        program = QStringLiteral("pkexec");
        args = {QStringLiteral("/bin/bash"), helper};
    } else if (havePkexec) {
        program = QStringLiteral("pkexec");
        args = {QStringLiteral("install"), QStringLiteral("-m"), QStringLiteral("0644"), rule,
                QStringLiteral("/etc/udev/rules.d/99-leica-dmc6200.rules")};
    } else {
        QMessageBox::information(parent, QObject::tr("Camera access"),
                                 QObject::tr("pkexec is not installed, so the rule cannot be installed from here.\n\n"
                                             "Run this in a terminal instead:\n\n    %1")
                                     .arg(helper.isEmpty()
                                              ? QStringLiteral("sudo install -m 0644 %1 /etc/udev/rules.d/").arg(rule)
                                              : QStringLiteral("sudo bash %1").arg(helper)));
        finished(QObject::tr("Install the udev rule manually"));
        return;
    }
    runInstaller(parent, program, args, QObject::tr("Camera access"),
                 QObject::tr("The access rule was installed. Unplug the camera and plug it back in."),
                 QObject::tr("The rule could not be installed. The details below say what went wrong; you can "
                             "also run driver/install_udev_rule.sh from a terminal."),
                 std::move(done));

#else
    QMessageBox::information(parent, QObject::tr("Camera access"),
                             QObject::tr("macOS needs no driver for the Leica camera: DM Imaging talks to it "
                                         "directly over USB.\n\nIf the camera is not found, unplug it for five "
                                         "seconds and plug it back in, and make sure no other imaging application "
                                         "has it open."));
    finished(QString());
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

bool moveImageToTrash(const QString &path)
{
    // the image first: its sidecars only follow when it really went, otherwise
    // an image that is still there would lose its metadata and annotations
    if (!QFile::moveToTrash(path))
        return false;
    for (const QString &sidecar : {path + QStringLiteral(".json"), path + QStringLiteral(".annotations.json")})
        if (QFileInfo::exists(sidecar) && !QFile::moveToTrash(sidecar))
            qWarning("could not move %s to the trash", qPrintable(sidecar));
    return true;
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
