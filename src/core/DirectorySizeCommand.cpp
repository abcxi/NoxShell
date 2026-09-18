#include "DirectorySizeCommand.h"

#include <QRegularExpression>
#include <limits>

namespace noxshell::detail {
QByteArray directorySizeCommand(const QString &path)
{
    if (!path.startsWith(QLatin1Char('/')) || path.contains(QChar::Null)) return {};
    auto quoted = path;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    // A separate exec channel, no terminal input and no remote temporary files.
    // EOF (navigation, cancellation or disconnect) stops the supervised process.
    // timeout also bounds the scan when the network disappears without an EOF.
    return (QStringLiteral(R"SH(# NOXSHELL_DIRECTORY_SIZE_V1
export LC_ALL=C
p=')SH") + quoted + QStringLiteral(R"SH('
case "$p" in /proc|/proc/*|/sys|/sys/*|/dev|/dev/*|/run|/run/*) echo 'Skipped virtual filesystem' >&2; exit 1;; esac
command -v du >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1 || { echo 'du/timeout unavailable; scan disabled' >&2; exit 1; }
exec 3<&0
scan_pid= watcher_pid=
stop_scan() {
    [ -z "$scan_pid" ] || kill -TERM "$scan_pid" 2>/dev/null
    [ -z "$watcher_pid" ] || kill -TERM "$watcher_pid" 2>/dev/null
    [ -z "$scan_pid" ] || wait "$scan_pid" 2>/dev/null
    [ -z "$watcher_pid" ] || wait "$watcher_pid" 2>/dev/null
    :
}
trap 'stop_scan; exit 130' HUP INT TERM
set -- sh -c '
p=$1
[ -d "$p" ] && [ ! -L "$p" ] || { echo "Not a regular directory" >&2; exit 1; }
fs=$(stat -f -c %T -- "$p" 2>/dev/null) || fs=
case "$fs" in nfs*|cifs|smb*|fuse*|proc|sysfs|devtmpfs|devpts) echo "Skipped remote or virtual filesystem" >&2; exit 1;; esac
exec du -skx -- "$p"
' noxshell-size "$p"
if command -v ionice >/dev/null 2>&1; then set -- ionice -c 3 "$@"; fi
if command -v nice >/dev/null 2>&1; then set -- nice -n 19 "$@"; fi
timeout -k 2 60 "$@" < /dev/null 3<&- &
scan_pid=$!
( IFS= read -r cancel <&3; kill -TERM "$scan_pid" 2>/dev/null; : ) &
watcher_pid=$!
wait "$scan_pid"
result=$?
scan_pid=
kill -TERM "$watcher_pid" 2>/dev/null
wait "$watcher_pid" 2>/dev/null
watcher_pid=
trap - HUP INT TERM
exit "$result"
)SH")).toUtf8();
}

bool parseDirectorySize(const QByteArray &output, quint64 &bytes)
{
    // Only the leading numeric field matters; filenames can contain newlines.
    static const QRegularExpression prefix(QStringLiteral("\\A([0-9]+)\\t"));
    const auto match = prefix.match(QString::fromUtf8(output));
    if (!match.hasMatch()) return false;
    bool ok = false;
    const auto kib = match.captured(1).toULongLong(&ok);
    if (!ok || kib > std::numeric_limits<quint64>::max() / 1024) return false;
    bytes = kib * 1024;
    return true;
}
}
