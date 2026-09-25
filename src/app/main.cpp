#include "app/AppSettings.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImageReader>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <windows.h>

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
    OutputDebugStringW(reinterpret_cast<const wchar_t *>(line.utf16()));
}

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
    QApplication::setApplicationVersion(QStringLiteral("1.0.1"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

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
    SetUnhandledExceptionFilter(crashHandler);
    qInfo("DM Imaging %s starting", qPrintable(QApplication::applicationVersion()));

    auto &S = lm::AppSettings::instance();
    S.load();
    lm::applyTheme(app, S.theme);

    lm::MainWindow w;
    w.showMaximized();
    QTimer::singleShot(100, &w, &lm::MainWindow::startup);
    const int rc = app.exec();
    qInfo("exit %d", rc);
    return rc;
}
