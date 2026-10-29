import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("packages", pathlib.Path(__file__).resolve().parents[1] / "tools/server_packages.py")
packages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packages)


class SoftwarePackagesTest(unittest.TestCase):
    def test_package_names_cannot_be_options_paths_patterns_or_removal_suffixes(self):
        for value in [[], ["--help"], ["nginx-"], ["git+"], ["../a"], ["./a.deb"], ["a;b"],
                      ["$(id)"], ["a\nb"], ["a*"], ["a=1"], ["a:amd64"], ["a/a"], ["a", "a"], ["a"]*11]:
            with self.assertRaises(RuntimeError):
                packages.names(value)
        self.assertEqual(packages.names(["libssl1.1", "python3-pip", "ImageMagick", "libstdc++6"]),
                         ["ImageMagick", "libssl1.1", "libstdc++6", "python3-pip"])

    def test_apt_search_is_offline_bounded_and_marks_only_installed_packages(self):
        text = "".join("tool%d - description\n" % i for i in range(100))
        with patch.object(packages, "manager", return_value="apt"), \
             patch.object(packages, "run", side_effect=[(text, False), ("tool0\tinstalled\ntool1\tconfig-files\n", False)]) as run:
            result = packages.search({"query": "tool"})
        self.assertEqual(len(result["rows"]), 50)
        self.assertTrue(result["partial"])
        self.assertTrue(result["rows"][0]["installed"])
        self.assertFalse(result["rows"][1]["installed"])
        self.assertEqual(run.call_args_list[0].args[0], ["apt-cache", "search", "--names-only", "tool"])
        self.assertNotIn("update", repr(run.call_args_list))

    def test_invalid_search_does_not_start_any_command(self):
        for query in ["--help", "x;id", "$(id)", "x\ny", "a"*65]:
            with patch.object(packages, "manager", return_value="apt"), patch.object(packages, "run") as run:
                with self.assertRaises(RuntimeError):
                    packages.search({"query": query})
                run.assert_not_called()

    def test_rpm_parser_skips_headers_and_handles_architecture_and_case(self):
        text = "Loaded plugins: fastestmirror\nAvailable Packages\nImageMagick.x86_64 7.1 repository\nnginx.noarch 1.0 repo\n"
        with patch.object(packages, "run", return_value=(text, False)):
            rows, cut = packages.available("dnf", selected=["ImageMagick", "nginx"])
        self.assertEqual([x["name"] for x in rows], ["ImageMagick", "nginx"])
        self.assertFalse(cut)
        with patch.object(packages, "run", return_value=("nginx.x86_64 : HTTP server\n", False)):
            self.assertEqual(packages.available("yum", query="nginx")[0][0]["description"], "HTTP server")

    def plan(self, which="apt"):
        with patch.object(packages, "manager", return_value=which), patch.object(packages.os, "geteuid", return_value=0), \
             patch.object(packages, "available", return_value=([{"name": "nginx"}], False)), \
             patch.object(packages.shutil, "which", return_value="/usr/bin/"+("apt-get" if which == "apt" else which)):
            return packages.plan({"manager": which, "packages": ["nginx"]})

    def test_plan_only_installs_selected_packages_without_sudo_or_forced_erasure(self):
        result, args = self.plan()
        self.assertIn("--no-remove", args)
        self.assertIn("DPkg::Lock::Timeout=0", args)
        self.assertEqual(args[-2:], ["install", "nginx"])
        self.assertEqual(len(result["plan"]), 64)
        self.assertNotIn("sudo", args)
        self.assertNotIn("upgrade", args)
        self.assertNotIn("--allow-unauthenticated", args)
        for which in ("dnf", "yum"):
            _, args = self.plan(which)
            self.assertEqual(args[-2:], ["install", "nginx"])
            self.assertNotIn("--allowerasing", args)
            self.assertIn("--setopt=gpgcheck=1", args)
            if which == "dnf":
                self.assertIn("--setopt=exit_on_lock=True", args)

    def test_plan_rejects_non_root_changed_manager_and_missing_repository_package(self):
        with patch.object(packages, "manager", return_value="apt"), patch.object(packages.os, "geteuid", return_value=1000):
            with self.assertRaises(RuntimeError):
                packages.plan({"manager": "apt", "packages": ["nginx"]})
        with patch.object(packages, "manager", return_value="dnf"):
            with self.assertRaises(RuntimeError):
                packages.plan({"manager": "apt", "packages": ["nginx"]})
        with patch.object(packages, "manager", return_value="apt"), patch.object(packages.os, "geteuid", return_value=0), \
             patch.object(packages, "available", return_value=([], False)):
            with self.assertRaises(RuntimeError):
                packages.plan({"manager": "apt", "packages": ["nginx"]})

    def test_install_requires_matching_plan_before_creating_a_job(self):
        result, args = self.plan()
        with patch.object(packages, "plan", return_value=(result, args)), patch.object(packages.os, "mkdir") as mkdir:
            with self.assertRaises(RuntimeError):
                packages.install({"plan": "wrong", "job": "a"*32})
            mkdir.assert_not_called()

    def test_private_job_records_reject_symlinks_and_do_not_delete_existing_logs(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(packages, "JOB_ROOT", directory):
            job = "a"*32
            os.mkdir(packages.job_path(job), 0o700)
            fd = packages.open_job(job)
            try:
                packages.save_state(fd, {"phase": "succeeded", "packages": ["nginx"]})
                packages.write_record(fd, "output.log", b"test-only-log")
                self.assertEqual(packages.status({"job": job})["log"], "test-only-log")
                os.unlink("output.log", dir_fd=fd)
                os.symlink("/etc/passwd", os.path.join(packages.job_path(job), "output.log"))
                with self.assertRaises(OSError):
                    packages.status({"job": job})
            finally:
                os.close(fd)
            for invalid in ["../x", "/root", "x"*32, "a"*32+"\n"]:
                with self.assertRaises(RuntimeError):
                    packages.job_path(invalid)

    def test_supervisor_bounds_logs_without_killing_successful_process(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(packages, "JOB_ROOT", directory):
            job = "b"*32
            os.mkdir(packages.job_path(job), 0o700)
            # A synthetic Python producer only. Never invoke a real package manager.
            packages.run_job(job, {}, [sys.executable, "-c", "import sys;sys.stdout.write('x'*500000);print('END')"])
            state = packages.status({"job": job})
            self.assertEqual(state["phase"], "succeeded")
            self.assertEqual(state["exitCode"], 0)
            self.assertLessEqual(len(state["log"]), packages.LOG_CAP)
            self.assertTrue(state["log"].endswith("END\n"))

    def test_log_write_failure_still_drains_process_to_completion(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(packages, "JOB_ROOT", directory):
            job = "c"*32
            os.mkdir(packages.job_path(job), 0o700)
            original = packages.write_record
            def write(fd, name, data):
                if name == "output.log":
                    raise OSError("synthetic disk full")
                return original(fd, name, data)
            with patch.object(packages, "write_record", side_effect=write):
                packages.run_job(job, {}, [sys.executable, "-c", "print('safe completion')"])
            fd = packages.open_job(job)
            try:
                state = json.loads(packages.read_record(fd, "state.json", 16384))
                self.assertEqual(state["phase"], "succeeded")
                self.assertIn("logWarning", state)
            finally:
                os.close(fd)

    def test_stale_supervisor_is_unknown_never_reported_as_cancelled(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(packages, "JOB_ROOT", directory):
            job = "d"*32
            os.mkdir(packages.job_path(job), 0o700)
            fd = packages.open_job(job)
            try:
                packages.save_state(fd, {"phase": "running", "pid": 123, "start": "old"})
                packages.write_record(fd, "output.log", b"partial")
            finally:
                os.close(fd)
            with patch.object(packages, "process_start", return_value="new"):
                self.assertEqual(packages.status({"job": job})["phase"], "unknown")

    def test_detached_job_survives_launch_process_and_rejects_duplicate_submission(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(packages, "JOB_ROOT", directory):
            job = "e"*32
            script = (
                "import importlib.util,sys,json;"
                "spec=importlib.util.spec_from_file_location('p',sys.argv[1]);p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p);"
                "p.JOB_ROOT=sys.argv[2];"
                "p.plan=lambda options: ({'plan':'ok','packages':['fixture']},[sys.executable,'-c',\"import time;time.sleep(.3);print('DETACHED-DONE')\"]);"
                "print(json.dumps(p.install({'plan':'ok','job':sys.argv[3]})),flush=True)"
            )
            # Only a fake Python producer is detached. stdout/stderr belong to
            # the short launch process, just like a completed SSH exec channel.
            result = subprocess.run([sys.executable, "-c", script, str(pathlib.Path(packages.__file__)), directory, job],
                                    input=b"", stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["job"], job)
            deadline = time.monotonic()+3
            while time.monotonic() < deadline:
                state = packages.status({"job": job})
                if state["phase"] == "succeeded":
                    break
                time.sleep(.02)
            self.assertEqual(state["phase"], "succeeded")
            self.assertIn("DETACHED-DONE", state["log"])
            with patch.object(packages, "plan", return_value=({"plan": "ok"}, [])), patch.object(packages.os, "fork") as fork:
                with self.assertRaises(RuntimeError):
                    packages.install({"plan": "ok", "job": job})
                fork.assert_not_called()

    def test_query_timeout_and_output_caps_apply_only_to_read_only_subprocess(self):
        with self.assertRaises(RuntimeError):
            packages.run([sys.executable, "-c", "import time;time.sleep(3)"], seconds=0.05)
        output, cut = packages.run([sys.executable, "-c", "print('x'*10000)"], cap=100)
        self.assertTrue(cut)
        self.assertEqual(len(output), 100)


if __name__ == "__main__":
    unittest.main()
