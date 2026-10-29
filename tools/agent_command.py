import base64
import json
import os
import selectors
import signal
import subprocess
import sys
import time

MAX_OUTPUT = 128 * 1024
TIMEOUT = 30

def main():
    command = base64.b64decode(sys.argv[1], validate=True).decode("utf-8")
    process = subprocess.Popen(
        ["sh", "-lc", command], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True, close_fds=True)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ, "stdout")
    selector.register(process.stderr, selectors.EVENT_READ, "stderr")
    selector.register(sys.stdin, selectors.EVENT_READ, "cancel")
    output = {"stdout": bytearray(), "stderr": bytearray()}
    deadline = time.monotonic() + TIMEOUT
    failure = ""
    try:
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                failure = "命令超过 30 秒，已终止"
                break
            for key, _ in selector.select(min(remaining, 0.25)):
                if key.data == "cancel":
                    if not os.read(sys.stdin.fileno(), 1):
                        failure = "命令已取消"
                        raise InterruptedError
                    continue
                chunk = os.read(key.fileobj.fileno(), 8192)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                target = output[key.data]
                available = MAX_OUTPUT - sum(map(len, output.values()))
                target.extend(chunk[:max(0, available)])
                if len(chunk) > available:
                    failure = "命令输出超过 128 KiB，已终止"
                    raise OverflowError
            if process.poll() is not None and len(selector.get_map()) == 1:
                break
    except (OverflowError, InterruptedError):
        pass
    finally:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    result = {
        "exitCode": process.returncode,
        "stdout": base64.b64encode(output["stdout"]).decode("ascii"),
        "stderr": base64.b64encode(output["stderr"]).decode("ascii"),
        "error": failure,
    }
    sys.stdout.write(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n")

if __name__ == "__main__":
    main()
