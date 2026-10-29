import base64
import json
import os
import pathlib
import subprocess
import sys
import time
import unittest

HELPER = pathlib.Path(__file__).resolve().parents[1] / "tools" / "agent_command.py"

class AgentCommandTest(unittest.TestCase):
    def launch(self, command):
        return subprocess.Popen([sys.executable, str(HELPER), base64.b64encode(command.encode()).decode()],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def result(self, process):
        raw = process.stdout.readline()
        process.wait(timeout=5)
        error = process.stderr.read().decode()
        self.assertEqual(process.returncode, 0, error)
        result = json.loads(raw)
        result["stdout"] = base64.b64decode(result["stdout"]).decode("utf-8", errors="replace")
        result["stderr"] = base64.b64decode(result["stderr"]).decode("utf-8", errors="replace")
        for stream in (process.stdin, process.stdout, process.stderr):
            if stream and not stream.closed:
                stream.close()
        return result

    def test_stdout_stderr_and_exit_status(self):
        result = self.result(self.launch("printf '中文 output'; printf 'stderr fixture' >&2; exit 7"))
        self.assertEqual(result["stdout"], "中文 output")
        self.assertEqual(result["stderr"], "stderr fixture")
        self.assertEqual(result["exitCode"], 7)

    def test_output_is_bounded(self):
        result = self.result(self.launch("python3 -c 'print(\"x\" * 300000)'"))
        self.assertLessEqual(len(result["stdout"].encode()) + len(result["stderr"].encode()), 128 * 1024)
        self.assertTrue(result["error"])

    def test_eof_cancels_child(self):
        process = self.launch("sleep 30")
        time.sleep(0.05)
        process.stdin.close()
        start = time.monotonic()
        result = self.result(process)
        self.assertLess(time.monotonic() - start, 3)
        self.assertIn("取消", result["error"])

if __name__ == "__main__":
    unittest.main()
