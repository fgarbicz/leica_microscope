#include "app/AppSettings.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/WheelGuard.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileOpenEvent>
#include <QIcon>
#include <QImageReader>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <csignal>
#include <cstring>
#include <unistd.h>
#endif

namespace {

QFile g_log;

void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    static const char *names[] = {"DEBUG", "WARN ", "CRIT ", "FATAL", "INFO "};
    const QString line = QStringLiteral("%1 %2 %3\n")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                             .arg(QString::fromLatin1(names[std::clamp(int(type), 0, 4)]))
                             .arg(msg);
    if (g_log.isOpen()) {
        g_log.write(line.toUtf8());
        g_log.flush();
    }
#ifdef Q_OS_WIN
    OutputDebugStringW(reinterpret_cast<const wchar_t *>(line.utf16()));
#else
    // stderr is where a console-launched build and the system log both look
    std::fputs(line.toLocal8Bit().constData(), stderr);
#endif
}

#ifdef Q_OS_WIN
LONG WINAPI crashHandler(EXCEPTION_POINTERS *info)
{
    if (g_log.isOpen()) {
        const QString line = QStringLiteral("FATAL unhandled exception 0x%1 at %2\n")
                                 .arg(info->ExceptionRecord->ExceptionCode, 8, 16, QLatin1Char('0'))
                                 .arg(quintptr(info->ExceptionRecord->ExceptionAddress), 0, 16);
        g_log.write(line.toUtf8());
        g_log.flush();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void installCrashHandler()
{
    SetUnhandledExceptionFilter(crashHandler);
}
#else
// A fatal signal must leave a trace in the support log. Only async-signal-safe
// calls are allowed here, so the line is written with write(2) to the log file
// descriptor rather than through QFile/qDebug.
int g_logFd = -1;

extern "C" void crashHandler(int sig)
{
    if (g_logFd >= 0) {
        const char *name = sig == SIGSEGV   ? "FATAL signal SIGSEGV (invalid memory access)\n"
                           : sig == SIGBUS  ? "FATAL signal SIGBUS\n"
                           : sig == SIGFPE  ? "FATAL signal SIGFPE\n"
                           : sig == SIGILL  ? "FATAL signal SIGILL\n"
                           : sig == SIGABRT ? "FATAL signal SIGABRT (aborted)\n"
                                            : "FATAL signal\n";
        const ssize_t ignored = ::write(g_logFd, name, std::strlen(name));
        (void)ignored;
    }
    // restore the default action and re-raise, so the OS crash reporter still runs
    std::signal(sig, SIG_DFL);
    ::raise(sig);
}

void installCrashHandler()
{
    g_logFd = g_log.isOpen() ? g_log.handle() : -1;
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
        std::signal(sig, crashHandler);
}
#endif

// The Finder does not pass the files to open as arguments: it sends them as
// QFileOpenEvents to the application, at launch ("Open with") and later.
class FileOpenFilter : public QObject {
public:
    explicit FileOpenFilter(lm::MainWindow *w) : m_window(w) {}

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::FileOpen) {
            const QString path = static_cast<QFileOpenEvent *>(e)->file();
            if (!path.isEmpty())
                m_window->openPaths({path});
            return true;
        }
        return QObject::eventFilter(o, e);
    }

private:
    lm::MainWindow *m_window;
};

} // namespace

int main(int argc, char **argv)
{
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    // Qt 6 refuses to decode images needing more than 256 MB by default; large
    // scans and stitched mosaics exceed that (memory failures are handled by the loaders)
    QImageReader::setAllocationLimit(4096);
    QApplication::setOrganizationName(QStringLiteral("DM Imaging"));
    QApplication::setApplicationName(QStringLiteral("DM Imaging"));
    QApplication::setApplicationVersion(QStringLiteral(DMI_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    // ties the windows to dmimaging.desktop (icon and name in the Wayland dock)
    QGuiApplication::setDesktopFileName(QStringLiteral("dmimaging"));

    // log file for support
    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(logDir);
    const QString logPath = logDir + QStringLiteral("/dmimaging.log");
    if (QFile::exists(logPath)) {
        // keep the previous session's log (e.g. after a crash) as dmimaging.1.log
        const QString prevPath = logDir + QStringLiteral("/dmimaging.1.log");
        QFile::remove(prevPath);
        QFile::rename(logPath, prevPath);
    }
    g_log.setFileName(logPath);
    if (g_log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        qInstallMessageHandler(messageHandler);
    installCrashHandler();
    qInfo("DM Imaging %s starting", qPrintable(QApplication::applicationVersion()));

    auto &S = lm::AppSettings::instance();
    S.load();
    lm::applyTheme(app, S.theme, S.uiScale);
    // the mouse wheel scrolls the panels; it never changes a value
    lm::WheelGuard::install(app);

    lm::MainWindow w;
    FileOpenFilter fileOpen(&w);
    app.installEventFilter(&fileOpen);
    w.showMaximized();
    QTimer::singleShot(100, &w, &lm::MainWindow::startup);
    // images passed on the command line: "Open with" on Windows, the %F of
    // the Linux .desktop entry. Qt has already removed its own options.
    QStringList files;
    for (const QString &a : app.arguments().mid(1))
        if (!a.startsWith(QLatin1Char('-')))
            files << a;
    if (!files.isEmpty())
        QTimer::singleShot(150, &w, [&w, files] { w.openPaths(files); });
    const int rc = app.exec();
    qInfo("exit %d", rc);
    return rc;
}
