#include "RemoteDirectoryFallback.h"

#include <QDateTime>

#include <utility>

namespace noxshell::detail {

namespace {

QByteArray quoteShellArgument(const QString &value)
{
    QByteArray quoted("'");
    const auto bytes = value.toUtf8();
    for (const char character : bytes) {
        if (character == '\'') quoted.append("'\\''");
        else quoted.append(character);
    }
    quoted.append('\'');
    return quoted;
}

} // namespace

QByteArray fallbackHomeDirectoryCommand()
{
    return QByteArrayLiteral(
        "export LC_ALL=C; "
        "if [ -n \"${HOME:-}\" ] && [ -d \"$HOME\" ]; then printf '%s\\n' \"$HOME\"; "
        "else pwd -P; fi");
}

QByteArray fallbackDirectoryListingCommand(const QString &path)
{
    return QByteArrayLiteral(
               "export LC_ALL=C; "
               "if ! command -v find >/dev/null 2>&1; then "
               "printf '远端缺少 find 命令\\n' >&2; exit 127; fi; "
               "find -- ")
        + quoteShellArgument(path)
        + QByteArrayLiteral(
            " -mindepth 1 -maxdepth 1 "
            "-printf '%f\\0%y\\0%s\\0%T@\\0%m\\0%u\\0%g\\0'");
}

bool parseFallbackDirectoryListing(const QString &path, const QByteArray &payload,
    RemoteFileEntries &entries, QString &failure)
{
    entries.clear();
    auto fields = payload.split('\0');
    while (!fields.isEmpty() && fields.last().isEmpty()) fields.removeLast();
    constexpr int fieldCount = 7;
    if (fields.size() % fieldCount != 0) {
        failure = QStringLiteral("兼容目录数据格式不完整");
        return false;
    }

    entries.reserve(fields.size() / fieldCount);
    for (int offset = 0; offset < fields.size(); offset += fieldCount) {
        const auto name = QString::fromUtf8(fields.at(offset));
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) continue;
        bool sizeValid = false;
        bool modifiedValid = false;
        bool modeValid = false;
        const quint64 size = fields.at(offset + 2).toULongLong(&sizeValid);
        const double modifiedSeconds = fields.at(offset + 3).toDouble(&modifiedValid);
        quint32 permissions = fields.at(offset + 4).toUInt(&modeValid, 8);
        if (!sizeValid || !modifiedValid || !modeValid) {
            failure = QStringLiteral("兼容目录数据包含无效属性");
            entries.clear();
            return false;
        }

        RemoteFileEntry entry;
        entry.name = name;
        entry.path = path == QStringLiteral("/") ? QStringLiteral("/") + name
                                                   : path + QLatin1Char('/') + name;
        entry.size = size;
        entry.modifiedAt = QDateTime::fromMSecsSinceEpoch(qRound64(modifiedSeconds * 1000.0));
        entry.owner = QString::fromUtf8(fields.at(offset + 5));
        entry.group = QString::fromUtf8(fields.at(offset + 6));
        const auto type = fields.at(offset + 1);
        entry.directory = type == QByteArrayLiteral("d");
        entry.symbolicLink = type == QByteArrayLiteral("l");
        permissions |= entry.directory ? 0040000U : entry.symbolicLink ? 0120000U : 0100000U;
        entry.permissions = permissions;
        entries.append(std::move(entry));
    }
    failure.clear();
    return true;
}

} // namespace noxshell::detail
