#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace noxshell {

// Monotonic, per-connection scheduling. No remote daemon, files or extra login.
class MetricsCollectionPolicy final {
public:
    QByteArray command(qint64 nowMs)
    {
        QByteArray result =
            "export LC_ALL=C; "
            "if command -v awk >/dev/null 2>&1; then "
            "awk 'FNR==1 { "
            "if (FILENAME==\"/proc/stat\") print \"__CPU__\"; "
            "else if (FILENAME==\"/proc/meminfo\") print \"__MEM__\"; "
            "else if (FILENAME==\"/proc/loadavg\") print \"__LOAD__\"; "
            "else if (FILENAME==\"/proc/uptime\") print \"__UPTIME__\"; "
            "else if (FILENAME==\"/proc/net/dev\") print \"__NET__\"; "
            "} FILENAME!=\"/proc/stat\" || /^cpu([0-9]+)? / { print }' "
            "/proc/stat /proc/meminfo /proc/loadavg /proc/uptime /proc/net/dev; else "
            "printf '__CPU__\\n'; grep -E '^cpu([0-9]+)? ' /proc/stat; "
            "printf '__MEM__\\n'; cat /proc/meminfo; "
            "printf '__LOAD__\\n'; cat /proc/loadavg; "
            "printf '__UPTIME__\\n'; cat /proc/uptime; "
            "printf '__NET__\\n'; cat /proc/net/dev; fi; ";
        if (m_lastProcesses < 0 || nowMs - m_lastProcesses >= 5000) {
            result += "printf '__PROC__\\n'; "
                "(ps -eo pid=,user=,pcpu=,pmem=,rss=,comm= --sort=-pcpu 2>/dev/null | head -n 12; "
                "ps -eo pid=,user=,pcpu=,pmem=,rss=,comm= --sort=-pmem 2>/dev/null | head -n 12); ";
            m_lastProcesses = nowMs;
        }
        if (m_lastDisks < 0 || nowMs - m_lastDisks >= 30000) {
            // Avoid touching slow/unavailable NFS/CIFS mounts during monitoring.
            result += "printf '__DISK__\\n'; df -Pkl 2>/dev/null; ";
            m_lastDisks = nowMs;
        }
        result += "true";
        return result;
    }

private:
    qint64 m_lastProcesses{-1};
    qint64 m_lastDisks{-1};
};

} // namespace noxshell
