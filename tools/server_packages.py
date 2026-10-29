"""Bounded repository queries and explicitly confirmed detached package installs.

No arbitrary shell commands, sudo, source changes, upgrades or forced cancellation.
Queries are bounded; installation owns a small private job directory and survives
the SSH channel closing. Only main() is exposed over the wire.
"""
import base64
import hashlib
import json
import os
import re
import selectors
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import time
import uuid

JOB_ROOT = "/tmp"
LOG_CAP = 32768
QUERY_CAP = 524288
FEATURED = ["curl", "wget", "git", "htop", "tmux", "vim", "nano", "nginx",
            "redis-server", "redis", "python3", "unzip", "jq", "rsync"]
NAME = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9+._-]{0,98}[A-Za-z0-9])?\Z")
JOB = re.compile(r"[0-9a-f]{32}\Z")
ARCHES = {"noarch", "x86_64", "aarch64", "i686", "i386", "ppc64le", "s390x", "armv7hl"}


def names(value):
    if not isinstance(value, list) or not 1 <= len(value) <= 10:
        raise RuntimeError("每次请选择 1–10 个软件包")
    if any(not isinstance(x, str) or not NAME.fullmatch(x) for x in value):
        raise RuntimeError("软件包名称无效；不接受路径、选项、版本表达式或安装/卸载后缀")
    if len(set(value)) != len(value):
        raise RuntimeError("软件包重复")
    return sorted(value)


def manager():
    if not sys.platform.startswith("linux"):
        raise RuntimeError("软件管理仅支持远程 Linux")
    if all(shutil.which(x) for x in ("apt-get", "apt-cache", "dpkg-query")):
        return "apt"
    if shutil.which("rpm"):
        for name in ("dnf", "yum"):
            if shutil.which(name):
                return name
    raise RuntimeError("当前仅支持 APT、DNF/YUM；未找到对应包管理器")


def run(args, seconds=4, cap=QUERY_CAP, allow_failure=False):
    """Read-only subprocess: bounded memory, time and process group lifetime."""
    child = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, close_fds=True, start_new_session=True,
                             env=dict(os.environ, LC_ALL="C", LANG="C", TERM="dumb", NO_COLOR="1"))
    data, cut = bytearray(), False
    deadline = time.monotonic() + seconds
    selector = selectors.DefaultSelector()
    selector.register(child.stdout, selectors.EVENT_READ)
    try:
        while True:
            if time.monotonic() >= deadline:
                raise RuntimeError("软件查询超时；未执行安装，请缩小关键词或检查本地仓库缓存")
            if not selector.select(0.05):
                continue
            block = os.read(child.stdout.fileno(), min(8192, cap - len(data) + 1))
            if not block:
                break
            data.extend(block)
            if len(data) > cap:
                del data[cap:]
                cut = True
                break
        if cut:
            os.killpg(child.pid, signal.SIGKILL)
        code = child.wait(timeout=max(0.1, deadline-time.monotonic()))
        text = data.decode("utf-8", "replace")
        if code and not allow_failure and not cut:
            raise RuntimeError(text[:500] or "仓库查询失败")
        return text, cut
    finally:
        selector.close()
        if child.poll() is None:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        child.wait(timeout=1)
        child.stdout.close()


def rpm_name(value):
    head, _, arch = value.rpartition(".")
    return head if arch in ARCHES else value


def available(which, query="", selected=None):
    if which == "apt":
        pattern = "^(" + "|".join(re.escape(x) for x in selected or FEATURED) + ")$" if selected or not query else re.escape(query)
        text, cut = run(["apt-cache", "search", "--names-only", pattern])
        rows = []
        for line in text.splitlines():
            package, sep, description = line.partition(" - ")
            if sep and NAME.fullmatch(package):
                rows.append({"name": package, "description": description[:240]})
        return rows, cut
    if selected or not query:
        args = [which, "-C", "--color=never", "list", "available"] + (selected or FEATURED)
    else:
        args = [which, "-C", "--color=never", "search", query]
    text, cut = run(args, allow_failure=True)
    rows = []
    for line in text.splitlines():
        match = re.match(r"^([A-Za-z0-9][A-Za-z0-9+._-]*)\s+(?::\s*(.*)|\S+\s+\S+\s*)$", line)
        if not match:
            continue
        package = rpm_name(match[1])
        if match[2] is None and package == match[1]:
            continue
        if NAME.fullmatch(package) and (not selected or package in selected):
            rows.append({"name": package, "description": (match[2] or "仓库软件包")[:240]})
    if not rows and any(x in text.lower() for x in ("error:", "cannot find", "no enabled repos", "no cache")):
        raise RuntimeError(text[:500])
    return rows, cut


def search(options):
    which = manager()
    query = options.get("query", "")
    if not isinstance(query, str) or len(query) > 64 or (query and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9+._ -]{0,63}", query)):
        raise RuntimeError("请输入最多 64 字符的软件包英文名称或关键词，不接受选项和命令")
    rows, cut = available(which, query.strip())
    unique = {row["name"]: row for row in rows}
    cut |= len(unique) > 50
    rows = [unique[x] for x in sorted(unique)[:50]]
    if rows:
        packages = [row["name"] for row in rows]
        if which == "apt":
            text, installed_cut = run(["dpkg-query", "-W", "-f=${Package}\t${db:Status-Status}\n"] + packages,
                                      seconds=3, allow_failure=True)
            installed = {line.split("\t")[0] for line in text.splitlines() if line.endswith("\tinstalled")}
        else:
            text, installed_cut = run(["rpm", "-q", "--qf", "NOXSHELL\t%{NAME}\n"] + packages,
                                      seconds=3, allow_failure=True)
            installed = {line[9:] for line in text.splitlines() if line.startswith("NOXSHELL\t")}
        if installed_cut:
            raise RuntimeError("已安装状态返回超限，请缩小查询")
        for row in rows:
            row["installed"] = row["name"] in installed
    return {"manager": which, "canInstall": os.geteuid() == 0, "rows": rows,
            "partial": cut, "featured": not bool(query.strip())}


def plan(options):
    which = manager()
    if options.get("manager") != which:
        raise RuntimeError("包管理器已变化，请重新查询后安装")
    packages = names(options.get("packages"))
    if os.geteuid() != 0:
        raise RuntimeError("当前仅支持 root 连接安装；不会自动 sudo 或索取密码")
    found, cut = available(which, selected=packages)
    if cut or set(packages) - {row["name"] for row in found}:
        raise RuntimeError("部分软件包已不在当前仓库缓存中，请重新搜索；不会安装任意脚本或本地文件")
    executable = shutil.which("apt-get" if which == "apt" else which)
    if which == "apt":
        args = [executable, "-y", "--no-remove", "--no-install-recommends",
                "-o", "DPkg::Lock::Timeout=0", "-o", "Dpkg::Options::=--force-confdef",
                "-o", "Dpkg::Options::=--force-confold", "install"] + packages
    else:
        args = [executable, "-y", "--color=never", "--setopt=gpgcheck=1", "install"] + packages
        if which == "dnf":
            args[3:3] = ["--setopt=exit_on_lock=True", "--setopt=install_weak_deps=False"]
    command = "DEBIAN_FRONTEND=noninteractive " + " ".join(shlex.quote(x) for x in args)
    digest = hashlib.sha256(json.dumps(args, ensure_ascii=True, separators=(",", ":")).encode()).hexdigest()
    return {"manager": which, "packages": packages, "command": command, "plan": digest}, args


def job_path(job):
    if not isinstance(job, str) or not JOB.fullmatch(job):
        raise RuntimeError("任务编号无效")
    return os.path.join(JOB_ROOT, "noxshell-install-"+job)


def open_job(job):
    fd = os.open(job_path(job), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    info = os.fstat(fd)
    if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        os.close(fd)
        raise RuntimeError("任务目录所有者或权限不正确")
    return fd


def write_record(fd, name, data):
    temp = ".write-"+uuid.uuid4().hex
    out = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=fd)
    try:
        with os.fdopen(out, "wb") as stream:
            stream.write(data)
        os.rename(temp, name, src_dir_fd=fd, dst_dir_fd=fd)
    finally:
        try:
            os.unlink(temp, dir_fd=fd)
        except FileNotFoundError:
            pass


def save_state(fd, state):
    write_record(fd, "state.json", json.dumps(state, ensure_ascii=True).encode())


def read_record(fd, name, cap):
    source = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
    with os.fdopen(source, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_size > cap:
            raise RuntimeError("任务记录无效或超出上限")
        return stream.read(cap)


def process_start(pid):
    try:
        with open("/proc/%d/stat" % pid, encoding="ascii") as stream:
            return stream.read(4096).rsplit(")", 1)[1].split()[19]
    except (OSError, IndexError, ValueError):
        return ""


def run_job(job, state, args):
    """Detached supervisor. Never kill a package manager on a UI/SSH timeout."""
    fd = open_job(job)
    child = None
    try:
        state.update(phase="running", pid=os.getpid(), start=process_start(os.getpid()))
        save_state(fd, state)
        env = dict(os.environ, DEBIAN_FRONTEND="noninteractive", LC_ALL="C", LANG="C", TERM="dumb", NO_COLOR="1")
        child = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, close_fds=True, start_new_session=True, env=env)
        tail, updated = bytearray(), 0.0
        selector = selectors.DefaultSelector()
        selector.register(child.stdout, selectors.EVENT_READ)
        try:
            while True:
                if not selector.select(0.5):
                    continue
                block = os.read(child.stdout.fileno(), 8192)
                if not block:
                    break
                tail.extend(block)
                del tail[:-LOG_CAP]
                if time.monotonic()-updated >= 1:
                    try:
                        write_record(fd, "output.log", bytes(tail))
                    except OSError:
                        state["logWarning"] = "日志写入失败，仍继续读取输出，不强制中断安装"
                    updated = time.monotonic()
            try:
                write_record(fd, "output.log", bytes(tail))
            except OSError:
                state["logWarning"] = "末尾日志写入失败，请结合包管理器状态检查"
            code = child.wait()
            state.update(phase="succeeded" if code == 0 else "failed", exitCode=code)
        finally:
            selector.close()
            child.stdout.close()
    except Exception as error:
        # If the package process may still be alive, keep an unknown state;
        # neither kill it nor falsely report that installation was cancelled.
        state.update(phase="unknown" if child and child.poll() is None else "failed", error=str(error)[:500])
    finally:
        save_state(fd, state)
        os.close(fd)


def install(options):
    result, args = plan(options)
    if options.get("plan") != result["plan"]:
        raise RuntimeError("安装计划已变化，请重新确认完整命令")
    job = options.get("job")
    path = job_path(job)
    try:
        os.mkdir(path, 0o700)
    except FileExistsError:
        raise RuntimeError("任务编号已存在，请刷新状态，避免重复安装")
    fd = open_job(job)
    state = dict(result, phase="starting", created=time.time())
    try:
        write_record(fd, "output.log", b"")
        save_state(fd, state)
    finally:
        os.close(fd)
    try:
        pid = os.fork()
    except OSError:
        fd = open_job(job)
        try:
            state.update(phase="failed", error="无法启动后台安装进程")
            save_state(fd, state)
        finally:
            os.close(fd)
        raise
    if pid == 0:
        try:
            os.setsid()
            if os.fork() > 0:
                os._exit(0)
            signal.signal(signal.SIGHUP, signal.SIG_IGN)
            null = os.open(os.devnull, os.O_RDWR)
            for number in range(3):
                os.dup2(null, number)
            os.closerange(3, int(os.sysconf("SC_OPEN_MAX")))
            try:
                os.nice(10)
            except OSError:
                pass
            run_job(job, state, args)
        finally:
            os._exit(0)
    os.waitpid(pid, 0)
    return {"job": job, "phase": "starting", "logPath": path+"/output.log"}


def status(options):
    job = options.get("job")
    fd = open_job(job)
    try:
        state = json.loads(read_record(fd, "state.json", 16384))
        state["log"] = read_record(fd, "output.log", LOG_CAP).decode("utf-8", "replace")
    finally:
        os.close(fd)
    if state["phase"] == "running" and (not state.get("start") or process_start(state.get("pid", 0)) != state["start"]):
        state.update(phase="unknown", error="安装监控进程已失联，包管理器可能仍在运行；请到终端确认，勿重复安装")
    if state["phase"] == "starting" and time.time()-state.get("created", 0) > 20:
        state.update(phase="unknown", error="未能确认后台进程状态，请到终端检查后再决定后续操作")
    return dict(state, job=job, logPath=job_path(job)+"/output.log")


def dispatch(options):
    operation = options.get("operation")
    if operation == "packages.search":
        return search(options)
    if operation == "packages.plan":
        return plan(options)[0]
    if operation == "packages.install":
        return install(options)
    if operation == "packages.status":
        return status(options)
    raise RuntimeError("不支持的软件管理操作")


def main():
    try:
        if not sys.platform.startswith("linux"):
            raise RuntimeError("软件管理仅支持远程 Linux")
        encoded = sys.argv[1]
        if len(encoded) > 4096:
            raise RuntimeError("请求过大")
        options = json.loads(base64.b64decode(encoded, validate=True))
        if not isinstance(options, dict):
            raise RuntimeError("无效请求")
        result = dispatch(options)
    except Exception as error:
        result = {"error": str(error)[:600]}
    print(json.dumps(result, ensure_ascii=True), flush=True)


if __name__ == "__main__":
    main()
