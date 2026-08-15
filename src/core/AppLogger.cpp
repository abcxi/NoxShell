#include "AppLogger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QRegularExpression>
#include <QStandardPaths>

#include <cstdio>
#include <cstdlib>

namespace noxshell {

namespace {
QMutex &logMutex()
{
    static QMutex mutex;
    return mutex;
}

QString defaultLogPath()
{
    auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty()) directory = QDir::home().filePath(QStringLiteral(".noxshell-ops"));
    directory = QDir(directory).filePath(QStringLiteral("logs"));
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("noxshell-ops.log"));
}

void rotateLogs(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || info.size() < 5 * 1024 * 1024) return;
    QFile::remove(path + QStringLiteral(".3"));
    QFile::rename(path + QStringLiteral(".2"), path + QStringLiteral(".3"));
    QFile::rename(path + QStringLiteral(".1"), path + QStringLiteral(".2"));
    QFile::rename(path, path + QStringLiteral(".1"));
}

QString levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return QStringLiteral("DEBUG");
    case QtInfoMsg: return QStringLiteral("INFO");
    case QtWarningMsg: return QStringLiteral("WARN");
    case QtCriticalMsg: return QStringLiteral("ERROR");
    case QtFatalMsg: return QStringLiteral("FATAL");
    }
    return QStringLiteral("INFO");
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const QMutexLocker locker(&logMutex());
    const auto clean = AppLogger::sanitize(message);
    const auto source = context.file
        ? QStringLiteral(" [%1:%2]").arg(QString::fromUtf8(context.file)).arg(context.line)
        : QString{};
    const auto line = QStringLiteral("%1 [%2] %3%4\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), levelName(type), clean, source);
    const auto path = AppLogger::logFilePath();
    rotateLogs(path);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) file.write(line.toUtf8());
    std::fputs(line.toLocal8Bit().constData(), stderr);
    std::fflush(stderr);
    if (type == QtFatalMsg) std::abort();
}
} // namespace

void AppLogger::install()
{
    qInstallMessageHandler(messageHandler);
}

QString AppLogger::logFilePath()
{
    static const QString path = defaultLogPath();
    return path;
}

QString AppLogger::sanitize(QString message)
{
    static const QRegularExpression assignments(
        QStringLiteral(R"((password|passphrase|authorization|token|secret)\s*([:=])\s*([^\s,;]+))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bearer(
        QStringLiteral(R"((Bearer\s+)[A-Za-z0-9._~+/=-]+)"), QRegularExpression::CaseInsensitiveOption);
    message.replace(assignments, QStringLiteral("\\1\\2<redacted>"));
    message.replace(bearer, QStringLiteral("\\1<redacted>"));
    return message;
}

} // namespace noxshell
