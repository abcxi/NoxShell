"""One bounded Linux management request; no daemon, install, sudo or temp files."""
import base64
import ipaddress
import json
import os
import re
import selectors
import signal
import subprocess
import sys
import threading
import time
import math

MAX_SCAN = 10000
children = set()
children_lock = threading.Lock()
cancelled = threading.Event()


def stop_children():
    with children_lock:
        for child in list(children):
            if child.poll() is None:
                kill_child(child)
                try:
                    child.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    pass


def kill_child(child):
    try:
        if sys.platform.startswith("linux"):
            os.killpg(child.pid, signal.SIGKILL)
        else:
            child.kill()
    except ProcessLookupError:
        pass


def watch_input():
    # SSH channel EOF means the window closed, cancel was clicked or SSH ended.
    try:
        os.read(0, 1)
    except OSError:
        pass
    cancelled.set()
    stop_children()
    os._exit(130)


def run(args, cap=262144, seconds=3, partial=False):
    """Drain pipes incrementally with hard byte/deadline limits."""
    with children_lock:
        if cancelled.is_set():
            raise RuntimeError("已取消")
        # A supervisor also bounds children if this Python process is killed.
        command = (["timeout", "-k", "1", str(math.ceil(seconds) + 1)] + args
                   if sys.platform.startswith("linux") else args)
        child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, env=dict(os.environ, LC_ALL="C"),
                                 start_new_session=sys.platform.startswith("linux"))
        children.add(child)
    data = bytearray()
    truncated = False
    deadline = time.monotonic() + seconds
    selector = selectors.DefaultSelector()
    selector.register(child.stdout, selectors.EVENT_READ)
    try:
        while True:
            if time.monotonic() > deadline:
                if not partial:
                    raise RuntimeError("命令超时，已停止")
                truncated = True
                break
            if not selector.select(min(0.1, max(0, deadline - time.monotonic()))):
                continue
            block = os.read(child.stdout.fileno(), min(8192, cap - len(data) + 1))
            if not block:
                break
            if len(data) + len(block) > cap:
                if not partial:
                    raise RuntimeError("输出超出安全上限")
                truncated = True
                break
            data.extend(block)
        if truncated:
            kill_child(child)
        status = child.wait(timeout=max(0.1, deadline - time.monotonic()))
        text = data.decode("utf-8", "replace")
        if status and not truncated:
            raise RuntimeError(text[:500] or "命令执行失败")
        if truncated:
            text = text.rsplit("\n", 1)[0] + "\n"
        return text, truncated
    finally:
        selector.close()
        with children_lock:
            if child.poll() is None:
                kill_child(child)
            child.wait(timeout=1)
            children.discard(child)
        child.stdout.close()


def interfaces():
    text, _ = run(["ip", "-j", "address", "show"])
    result = []
    for interface in json.loads(text)[:256]:
        for address in interface.get("addr_info", [])[:32]:
            value = address.get("local", "")
            try:
                ip = ipaddress.ip_address(value)
            except ValueError:
                continue
            if ip.is_loopback or ip.is_link_local or ip.is_unspecified:
                continue
            result.append({"name": interface.get("ifname", "")[:64], "ip": str(ip),
                           "private": ip.is_private})
    return {"rows": result[:512]}


def proc_stat(pid):
    with open("/proc/%d/stat" % pid, "r") as handle:
        raw = handle.read(4096)
    end = raw.rfind(")")
    fields = raw[end + 2:].split()
    return {"pid": pid, "name": raw[raw.find("(") + 1:end][:128],
            "state": fields[0], "ppid": int(fields[1]), "ticks": int(fields[11]) + int(fields[12]),
            "start": fields[19], "rss": max(0, int(fields[21])) * os.sysconf("SC_PAGE_SIZE"),
            "kernel": bool(int(fields[6]) & 0x00200000)}


def protected_process(row):
    if row["pid"] <= 2 or row.get("kernel") or row["name"].startswith(("sshd", "dropbear")) or row["name"] in {"systemd", "init"}:
        return True
    current = os.getpid()
    ssh_parent = None
    for _ in range(128):
        if row["pid"] == current:
            return True
        if current <= 1:
            break
        try:
            ancestor = proc_stat(current)
            if ssh_parent is None and ancestor["name"].startswith(("sshd", "dropbear")):
                ssh_parent = current
            current = ancestor["ppid"]
        except (OSError, ValueError, IndexError):
            return True  # Cannot verify the protection chain safely.
    else:
        return True
    # A PTY shell is a sibling of the exec helper, not its ancestor. Protect
    # the SSH connection subtree as well. Unverifiable chains fail closed.
    current_row = row
    if ssh_parent is None:
        return True
    if ssh_parent is not None:
        for _ in range(128):
            if current_row["pid"] == ssh_parent or current_row["name"].startswith(("sshd", "dropbear")):
                return True
            if current_row["ppid"] <= 1:
                break
            try:
                current_row = proc_stat(current_row["ppid"])
            except (OSError, ValueError, IndexError):
                return True
        else:
            return True
    return False


def processes(options):
    limit = max(10, min(500, int(options.get("limit", 100))))
    rows = {}
    clipped = False
    visited = 0
    with os.scandir("/proc") as entries:
        for entry in entries:
            if not entry.name.isdecimal():
                continue
            visited += 1
            if visited > MAX_SCAN:
                clipped = True
                break
            try:
                row = proc_stat(int(entry.name))
                rows[row["pid"]] = row
            except PermissionError:
                clipped = True
            except (OSError, ValueError, IndexError):
                continue
    started = time.monotonic()
    time.sleep(0.7)
    hz = os.sysconf("SC_CLK_TCK")
    result = []
    for old in rows.values():
        try:
            row = proc_stat(old["pid"])
            if row["start"] != old["start"]:
                continue
            row["cpu"] = max(0, row["ticks"] - old["ticks"]) * 100 / hz / max(0.1, time.monotonic() - started)
            row["protected"] = row["pid"] <= 2 or row.get("kernel") or row["name"] in {"sshd", "sshd-session", "sshd-auth", "systemd", "init"}
            result.append(row)
        except PermissionError:
            clipped = True
        except (OSError, ValueError, IndexError):
            continue
    key = "rss" if options.get("sort") == "memory" else "cpu"
    result.sort(key=lambda row: (row[key], row["pid"]), reverse=True)
    safe_signals = False
    if hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"):
        try:
            fd = os.pidfd_open(os.getpid())
            os.close(fd)
            safe_signals = True
        except OSError:
            pass
    return {"rows": result[:limit], "partial": clipped, "scanned": len(rows), "safeSignals": safe_signals}


def signal_process(options):
    pid = int(options["pid"])
    action = options["action"]
    signals = {"TERM": signal.SIGTERM, "KILL": signal.SIGKILL,
               "STOP": signal.SIGSTOP, "CONT": signal.SIGCONT}
    if action not in signals or pid <= 1:
        raise RuntimeError("无效或受保护的进程")
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        raise RuntimeError("此内核/Python 不支持安全 PID 句柄；仅允许查看，拒绝有 PID 复用风险的操作")
    fd = os.pidfd_open(pid)
    try:
        row = proc_stat(pid)
        if row["start"] != str(options["start"]) or protected_process(row):
            raise RuntimeError("进程已变化、属于系统/SSH 保护链，或无法安全核实保护链，操作已拒绝")
        signal.pidfd_send_signal(fd, signals[action])
    finally:
        os.close(fd)
    return {"message": "信号已发送；请刷新确认进程状态"}


def endpoint(word):
    try:
        host, port = word.rsplit(":", 1)
        host = host.strip("[]").split("%", 1)[0]
        ip = ipaddress.ip_address(host)
        if getattr(ip, "ipv4_mapped", None):
            ip = ip.ipv4_mapped
        if not (port.isdecimal() or port == "*"):
            return None
        return str(ip), port
    except ValueError:
        return None


def sockets():
    text, clipped = run(["ss", "-H", "-O", "-t", "-i", "-n", "-e", "state", "established"],
                        cap=4 * 1024 * 1024, seconds=3, partial=True)
    rows = {}
    for index, line in enumerate(text.splitlines()):
        if index >= MAX_SCAN:
            clipped = True
            break
        addresses = [value for word in line.split() if (value := endpoint(word)) is not None]
        if len(addresses) < 2:
            continue
        local, peer = addresses[:2]
        identity = re.search(r"\bsk:(\S+)", line) or re.search(r"\bino:(\S+)", line)
        # Without a stable socket identity, do not claim a bandwidth estimate.
        sent = re.search(r"\bbytes_sent:(\d+)", line) or re.search(r"\bbytes_acked:(\d+)", line)
        received = re.search(r"\bbytes_received:(\d+)", line)
        key = (local, peer, identity.group(1) if identity else "")
        rows[key] = {"local": local, "peer": peer,
                     "tx": int(sent.group(1)) if sent and identity else None,
                     "rx": int(received.group(1)) if received and identity else None}
    return rows, clipped


def traffic(options):
    limit = max(10, min(100, int(options.get("limit", 10))))
    listening, partial = run(["ss", "-H", "-l", "-t", "-n"], cap=262144, partial=True)
    ports = set()
    lines = listening.splitlines()
    partial = partial or len(lines) > 4096
    for line in lines[:4096]:
        words = line.split()
        # The local address follows Recv-Q/Send-Q, optionally after LISTEN.
        offset = 1 if words and words[0] == "LISTEN" else 0
        if len(words) > offset + 2:
            ports.add(words[offset + 2].rsplit(":", 1)[-1])
    first, cut1 = sockets()
    started = time.monotonic()
    time.sleep(0.8)
    second, cut2 = sockets()
    elapsed = max(0.1, time.monotonic() - started)
    groups = {}
    for key, row in second.items():
        if row["local"][1] not in ports:
            continue
        if ipaddress.ip_address(row["peer"][0]).is_loopback:
            continue
        group = groups.setdefault(row["peer"][0], {"ip": row["peer"][0], "connections": 0,
                                  "tx": 0, "rx": 0, "measured": 0})
        group["connections"] += 1
        old = first.get(key)
        if old and row["tx"] is not None and row["rx"] is not None and old["tx"] is not None and old["rx"] is not None:
            if row["tx"] >= old["tx"] and row["rx"] >= old["rx"]:
                group["tx"] += (row["tx"] - old["tx"]) / elapsed
                group["rx"] += (row["rx"] - old["rx"]) / elapsed
                group["measured"] += 1
    rows = list(groups.values())
    for row in rows:
        row["complete"] = row["measured"] == row["connections"]
        if not row["measured"]:
            row["tx"] = row["rx"] = None
    bandwidth = options.get("sort") == "bandwidth"
    rows.sort(key=lambda row: ((row["tx"] or 0) + (row["rx"] or 0) if bandwidth else row["connections"],
                               row["connections"]), reverse=True)
    return {"rows": rows[:limit], "clients": len(rows), "scanned": len(second),
            "partial": partial or cut1 or cut2}


def speed(options):
    target = str(options.get("target", ""))
    if len(target) > 253 or not target or target.startswith("-") or not re.fullmatch(r"[A-Za-z0-9_.:\-]+", target):
        raise RuntimeError("请输入有效的 iperf3 主机名或 IP，不接受 URL/命令")
    source = str(options.get("source", ""))
    if source and source not in {row["ip"] for row in interfaces()["rows"]}:
        raise RuntimeError("所选网卡地址已失效，请重新读取网卡")
    port = int(options.get("port", 5201))
    mbps = int(options.get("mbps", 10))
    if not 1 <= port <= 65535 or not 1 <= mbps <= 50:
        raise RuntimeError("端口/限速超出允许范围")
    args = ["iperf3", "-c", target, "-p", str(port), "-t", "3", "-P", "1", "-b", str(mbps) + "M", "-J"]
    if source:
        args += ["-B", source]
    if options.get("reverse", False):
        args.append("-R")
    text, _ = run(args, seconds=8)
    result = json.loads(text)
    if result.get("error"):
        raise RuntimeError(result["error"])
    end = result.get("end", {})
    return {"message": "3 秒单路 TCP 测试完成", "sent": end.get("sum_sent", {}).get("bits_per_second"),
            "received": end.get("sum_received", {}).get("bits_per_second")}


def dispatch(options):
    operation = options.get("operation")
    handlers = {"interfaces": lambda _: interfaces(), "processes": processes,
                "signal": signal_process, "traffic": traffic, "speed": speed}
    if operation not in handlers:
        raise RuntimeError("未知管理操作")
    return handlers[operation](options)


def main():
    try:
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (256 * 1024 * 1024, 256 * 1024 * 1024))
        resource.setrlimit(resource.RLIMIT_CPU, (5, 6))
        os.nice(10)
    except (ImportError, OSError, ValueError):
        pass
    def alarm_handler(*_):
        raise TimeoutError("管理任务超过 12 秒，已停止")
    signal.signal(signal.SIGALRM, alarm_handler)
    signal.alarm(12)
    threading.Thread(target=watch_input, daemon=True).start()
    try:
        options = json.loads(base64.b64decode(sys.argv[1], validate=True))
        result = dispatch(options)
    except Exception as error:
        result = {"error": str(error)[:500]}
    finally:
        stop_children()
        signal.alarm(0)
    print(json.dumps(result, ensure_ascii=True, allow_nan=False))


if __name__ == "__main__":
    main()
