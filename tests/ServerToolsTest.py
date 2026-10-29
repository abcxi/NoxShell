import importlib.util
import json
import pathlib
import sys
import time
import unittest
from unittest.mock import patch, Mock, MagicMock
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location("tools", pathlib.Path(__file__).resolve().parents[1] / "tools/server_tools.py")
tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tools)


class ServerToolsTest(unittest.TestCase):
    def test_endpoint_ipv4_ipv6_and_non_addresses(self):
        self.assertEqual(tools.endpoint("192.0.2.1:443"), ("192.0.2.1", "443"))
        self.assertEqual(tools.endpoint("[::ffff:192.0.2.1]:80"), ("192.0.2.1", "80"))
        self.assertEqual(tools.endpoint("[2001:db8::1]:443"), ("2001:db8::1", "443"))
        self.assertIsNone(tools.endpoint("rtt:20/0.2"))

    def test_ss_parses_counters_and_never_fabricates_missing_ones(self):
        text = ("0 0 10.0.0.1:443 192.0.2.2:2000 ino:123 sk:ab bytes_sent:1024 bytes_received:512\n"
                "0 0 [::1]:443 [::1]:2001 ino:124 sk:ac rtt:20/0.2\n")
        with patch.object(tools, "run", return_value=(text, False)):
            rows, cut = tools.sockets()
        self.assertFalse(cut)
        self.assertEqual(len(rows), 2)
        self.assertEqual(list(rows.values())[0]["tx"], 1024)
        self.assertIsNone(list(rows.values())[1]["rx"])

    def test_traffic_aggregates_clients_and_sorts_top_n(self):
        first, second = {}, {}
        for i in range(200):
            key = (("10.0.0.1", "443"), ("192.0.2.1", str(1000 + i)), str(i))
            first[key] = {"local": key[0], "peer": key[1], "tx": 0, "rx": 0}
            second[key] = dict(first[key], tx=10, rx=20)
        for i in range(20):
            key = (("10.0.0.1", "443"), ("198.51.100.%d" % (i + 1), "2000"), "other%d" % i)
            first[key] = {"local": key[0], "peer": key[1], "tx": 0, "rx": 0}
            second[key] = dict(first[key], tx=100000 + i, rx=0)
        def result(sort):
            with patch.object(tools, "run", return_value=("LISTEN 0 128 *:443 *:*\n", False)), \
                 patch.object(tools, "sockets", side_effect=[(first, False), (second, True)]), \
                 patch.object(tools.time, "sleep"), patch.object(tools.time, "monotonic", side_effect=[10, 11]):
                return tools.traffic({"limit": 10, "sort": sort})
        connections = result("connections")
        self.assertEqual(len(connections["rows"]), 10)
        self.assertEqual(connections["rows"][0]["connections"], 200)
        self.assertEqual(connections["rows"][0]["tx"], 2000)
        self.assertTrue(connections["partial"])
        self.assertEqual(result("bandwidth")["rows"][0]["ip"], "198.51.100.20")

    def test_speed_has_explicit_bounded_arguments(self):
        response = json.dumps({"end": {"sum_sent": {"bits_per_second": 1000}, "sum_received": {"bits_per_second": 900}}})
        with patch.object(tools, "interfaces", return_value={"rows": [{"ip": "10.0.0.2"}]}), \
             patch.object(tools, "run", return_value=(response, False)) as run:
            result = tools.speed({"target": "192.0.2.1", "source": "10.0.0.2", "mbps": 10, "reverse": True})
        args = run.call_args[0][0]
        self.assertEqual(args[args.index("-t") + 1], "3")
        self.assertEqual(args[args.index("-P") + 1], "1")
        self.assertEqual(args[args.index("-b") + 1], "10M")
        self.assertEqual(args[args.index("-B") + 1], "10.0.0.2")
        self.assertIn("-R", args)
        self.assertEqual(result["received"], 900)

    def test_speed_rejects_injection_invalid_source_and_limits(self):
        for target in ["x;touch /tmp/x", "$(id)", "https://a", "--help", "a b"]:
            with self.assertRaises(RuntimeError):
                tools.speed({"target": target})
        with self.assertRaises(RuntimeError):
            tools.speed({"target": "localhost", "mbps": 51})
        with patch.object(tools, "interfaces", return_value={"rows": []}):
            with self.assertRaises(RuntimeError):
                tools.speed({"target": "localhost", "source": "10.0.0.2"})

    def test_process_action_rejects_pid_one_and_stale_identity(self):
        with self.assertRaises(RuntimeError):
            tools.signal_process({"pid": 1, "action": "KILL", "start": "1"})
        with patch.object(tools.os, "pidfd_open", return_value=99, create=True), \
             patch.object(tools.signal, "pidfd_send_signal", create=True) as send, \
             patch.object(tools.os, "close"), patch.object(tools, "proc_stat", return_value={"start": "new"}):
            with self.assertRaises(RuntimeError):
                tools.signal_process({"pid": 200, "action": "TERM", "start": "old"})
            send.assert_not_called()

    def test_channel_eof_stops_owned_children(self):
        child = Mock()
        child.poll.return_value = None
        with patch.object(tools, "children", {child}), patch.object(tools, "cancelled", tools.threading.Event()), patch.object(tools.os, "read", return_value=b""), \
             patch.object(tools, "kill_child") as kill, patch.object(tools.os, "_exit") as exit:
            tools.watch_input()
            kill.assert_called_once_with(child)
            child.wait.assert_called_once_with(timeout=1)
            exit.assert_called_once_with(130)
            self.assertTrue(tools.cancelled.is_set())

    def test_process_action_uses_verified_pidfd_and_rejects_ssh(self):
        for name, permitted in [("worker", True), ("sshd", False)]:
            row = {"pid": 200, "name": name, "start": "42", "ppid": 1}
            with patch.object(tools.os, "pidfd_open", return_value=99, create=True), \
                 patch.object(tools.signal, "pidfd_send_signal", create=True) as send, \
                 patch.object(tools.os, "close"), patch.object(tools.os, "getpid", return_value=900), \
                 patch.object(tools, "proc_stat", side_effect=lambda pid: row if pid == 200 else
                              {"pid": pid, "ppid": 901 if pid == 900 else 1,
                               "name": "python3" if pid == 900 else "sshd"}):
                if permitted:
                    tools.signal_process({"pid": 200, "action": "TERM", "start": "42"})
                    send.assert_called_once_with(99, tools.signal.SIGTERM)
                else:
                    with self.assertRaises(RuntimeError):
                        tools.signal_process({"pid": 200, "action": "KILL", "start": "42"})
                    send.assert_not_called()

    def test_current_ssh_pty_sibling_is_protected(self):
        rows = {300: {"pid": 300, "ppid": 200, "name": "python3"},
                200: {"pid": 200, "ppid": 100, "name": "sshd-session"},
                100: {"pid": 100, "ppid": 1, "name": "sshd"}}
        with patch.object(tools.os, "getpid", return_value=300), \
             patch.object(tools, "proc_stat", side_effect=lambda pid: rows[pid]):
            self.assertTrue(tools.protected_process({"pid": 400, "ppid": 200, "name": "bash"}))
            self.assertFalse(tools.protected_process({"pid": 500, "ppid": 1, "name": "worker"}))
        with patch.object(tools.os, "getpid", return_value=300), \
             patch.object(tools, "proc_stat", side_effect=PermissionError("hidden")):
            self.assertTrue(tools.protected_process({"pid": 500, "ppid": 1, "name": "worker"}))

    def test_process_scan_is_bounded_before_sorting(self):
        entries = MagicMock()
        entries.__enter__.return_value = iter([SimpleNamespace(name=str(i)) for i in range(100, 110)])
        def row(pid):
            return {"pid": pid, "name": "worker", "state": "S", "ppid": 1, "ticks": 100,
                    "start": "42", "rss": 1024, "kernel": False}
        with patch.object(tools.os, "scandir", return_value=entries), patch.object(tools, "MAX_SCAN", 3), \
             patch.object(tools, "proc_stat", side_effect=row), patch.object(tools.time, "sleep"):
            result = tools.processes({"limit": 500})
        self.assertTrue(result["partial"])
        self.assertEqual(result["scanned"], 3)
        self.assertEqual(len(result["rows"]), 3)

    @unittest.skipIf(sys.platform == "win32", "Unix pipe selector")
    def test_subprocess_output_and_time_are_bounded(self):
        with self.assertRaises(RuntimeError):
            tools.run([sys.executable, "-c", "print('x'*100000)"], cap=1024)
        started = time.monotonic()
        with self.assertRaises(RuntimeError):
            tools.run([sys.executable, "-c", "import time;time.sleep(10)"], seconds=0.1)
        self.assertLess(time.monotonic() - started, 2)
        self.assertFalse(tools.children)


if __name__ == "__main__":
    unittest.main(verbosity=2)
