#!/usr/bin/env python3
"""Bounded Linux supervision fixtures; no compiler, build, installation or Git."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest import mock

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location(
    "nominal_delivery", Path(__file__).with_name("deliver-generic-nominals.py"))
DELIVERY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DELIVERY)

FIXTURE = r'''
import json
import os
from pathlib import Path
import subprocess
import sys
import time

role, mode, directory = sys.argv[1:]
root = Path(directory)
fields = Path(f"/proc/{os.getpid()}/stat").read_text().rsplit(") ", 1)[1].split()
pending = root / (role + ".pending")
with pending.open("x") as record:
    json.dump({"pid": os.getpid(), "start": fields[19], "session": os.getsid(0)}, record)
pending.rename(root / (role + ".json"))

def ready(name):
    deadline = time.monotonic() + 10
    while not (root / name).exists():
        if time.monotonic() >= deadline:
            raise RuntimeError("fixture readiness deadline")
        time.sleep(0.005)

if role == "leaf":
    if mode == "success":
        print("leaf", flush=True)
    else:
        time.sleep(60)
elif role == "middle":
    child = subprocess.Popen([sys.executable, "-B", __file__, "leaf", mode, directory],
                             start_new_session=True)
    ready("leaf.json")
    if mode == "orphan":
        os._exit(0)
    raise SystemExit(child.wait())
else:
    child = subprocess.Popen([sys.executable, "-B", __file__, "middle", mode, directory],
                             start_new_session=True)
    ready("leaf.json")
    (root / "ready").write_text("ready")
    if mode == "failed-wrapper":
        print("wrapper stdout", flush=True)
        print("wrapper stderr", file=sys.stderr, flush=True)
        raise SystemExit(7)
    raise SystemExit(child.wait())
'''


@unittest.skipUnless(sys.platform == "linux" and hasattr(os, "pidfd_open")
                     and hasattr(signal, "pidfd_send_signal"), "Linux pidfds required")
class SupervisionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="axeon-delivery-supervisor-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fixture = self.root / "process-fixture.py"
        self.fixture.write_text(FIXTURE, encoding="utf-8")
        self.addCleanup(self.cleanup_fixture_processes)

    def cleanup_fixture_processes(self):
        # Failure containment for the tests themselves: only exact identities
        # written by this fresh fixture, checked again after opening a pidfd.
        for path in self.root.glob("*.json"):
            value = json.loads(path.read_text())
            try:
                handle = os.pidfd_open(value["pid"])
            except ProcessLookupError:
                continue
            try:
                stat = Path(f"/proc/{value['pid']}/stat").read_text().rsplit(") ", 1)[1].split()
                if stat[19] == value["start"]:
                    signal.pidfd_send_signal(handle, signal.SIGKILL)
            except (FileNotFoundError, ProcessLookupError):
                pass
            finally:
                os.close(handle)

    def command(self, mode):
        return [sys.executable, "-B", str(self.fixture), "root", mode, str(self.root)]

    def assert_fixture_gone(self):
        records = [json.loads((self.root / f"{name}.json").read_text())
                   for name in ("root", "middle", "leaf")]
        self.assertEqual(len({record["session"] for record in records}), 3)
        for record in records:
            self.assertFalse(Path(f"/proc/{record['pid']}").exists(), record)

    def test_success_preserves_stdout_stderr_and_reaps_nested_sessions(self):
        checks = []
        self.assertEqual(DELIVERY.run(self.command("success"), self.root, timeout=15,
                                     expected=b"leaf\n", record=checks), b"leaf\n")
        self.assertEqual(len(checks), 1)
        self.assertEqual(checks[0]["command"], self.command("success"))
        self.assertEqual(checks[0]["exit_code"], 0)
        self.assertEqual(checks[0]["stderr"], "")
        self.assert_fixture_gone()

    def test_timeout_reaps_nested_sessions_without_touching_unrelated_child(self):
        sibling = subprocess.Popen([sys.executable, "-B", "-c", "import time; time.sleep(60)"],
                                   start_new_session=True, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        try:
            started = time.monotonic()
            with self.assertRaises(subprocess.TimeoutExpired):
                DELIVERY.run(self.command("timeout"), self.root, timeout=3)
            self.assertLess(time.monotonic() - started, 3 + DELIVERY.CLEANUP_SECONDS + 5)
            self.assert_fixture_gone()
            self.assertIsNone(sibling.poll(), "unrelated direct child must survive")
        finally:
            if sibling.poll() is None:
                sibling.kill()
            sibling.wait(timeout=5)

    def test_nonzero_wrapper_exit_reaps_adopted_children_before_return(self):
        checks = []
        with self.assertRaisesRegex(RuntimeError, "command failed"):
            DELIVERY.run(self.command("failed-wrapper"), self.root, timeout=15, record=checks)
        self.assertEqual(checks[0]["exit_code"], 7)
        self.assertEqual(checks[0]["stdout"], "wrapper stdout\n")
        self.assertEqual(checks[0]["stderr"], "wrapper stderr\n")
        self.assert_fixture_gone()

    def test_signal_exit_preserves_exact_return_code_and_output(self):
        checks = []
        command = [sys.executable, "-B", "-c",
                   "import os,signal,sys; print('before signal',flush=True); "
                   "print('signal stderr',file=sys.stderr,flush=True); os.kill(os.getpid(),signal.SIGTERM)"]
        with self.assertRaisesRegex(RuntimeError, "command failed"):
            DELIVERY.run(command, self.root, timeout=15, record=checks)
        self.assertEqual(checks[0]["exit_code"], -signal.SIGTERM)
        self.assertEqual(checks[0]["stdout"], "before signal\n")
        self.assertEqual(checks[0]["stderr"], "signal stderr\n")

    def test_bounded_receipts_do_not_truncate_successful_command_output(self):
        checks = []
        size = DELIVERY.MAX_DIAGNOSTIC + 100
        expected = b"x" * size
        command = [sys.executable, "-B", "-c", f"import sys; sys.stdout.buffer.write(b'x'*{size})"]
        self.assertEqual(DELIVERY.run(command, self.root, timeout=15, expected=expected,
                                     record=checks), expected)
        self.assertEqual(checks[0]["stdout_bytes"], size)
        self.assertEqual(len(checks[0]["stdout"]), DELIVERY.MAX_DIAGNOSTIC)
        self.assertTrue(checks[0]["stdout_truncated"])
        self.assertEqual(checks[0]["stderr"], "")

    def test_successful_double_fork_cannot_abandon_work_and_claim_success(self):
        checks = []
        with self.assertRaisesRegex(RuntimeError, "command failed"):
            DELIVERY.run(self.command("orphan"), self.root, timeout=15, record=checks)
        self.assertEqual(checks[0]["exit_code"], 125)
        self.assertIn("surviving descendants", checks[0]["stderr"])
        self.assert_fixture_gone()

    def test_interruption_uses_the_same_owned_cleanup(self):
        original_popen = subprocess.Popen

        def interrupting_popen(*args, **kwargs):
            process = original_popen(*args, **kwargs)
            communicate = process.communicate
            first = True

            def interrupt_once(*arguments, **options):
                nonlocal first
                if first:
                    first = False
                    deadline = time.monotonic() + 10
                    while not (self.root / "ready").exists():
                        if time.monotonic() >= deadline:
                            raise RuntimeError("fixture did not become ready")
                        time.sleep(0.005)
                    raise KeyboardInterrupt
                return communicate(*arguments, **options)

            process.communicate = interrupt_once
            return process

        with mock.patch.object(DELIVERY.subprocess, "Popen", side_effect=interrupting_popen):
            with self.assertRaises(KeyboardInterrupt):
                DELIVERY.run(self.command("timeout"), self.root, timeout=15)
        self.assert_fixture_gone()


class SourceAllowanceTests(unittest.TestCase):
    def compare(self, changes, allowed):
        delivery = DELIVERY.Delivery.__new__(DELIVERY.Delivery)
        delivery.snapshot = Path("/fixture/delivery")
        delivery.functional_snapshot = Path("/fixture/functional")
        delivery.args = types.SimpleNamespace(allow_post_validation=allowed)
        old = {"docs/diagnostic-explanations.tsv": "d" * 64,
               "scripts/deliver-generic-nominals.py": "a" * 64}
        current = {**old, **changes}

        def manifest(hashes):
            return {"files": [{"path": path, "sha256": digest, "mode": "100644"}
                              for path, digest in hashes.items()]}, hashes

        delivery.index = lambda path: manifest(current if path == delivery.snapshot else old)
        delivery.committed_files = lambda: (current, {path: "100644" for path in current})

        def directory_hashes(directory):
            prefix = directory.name + "/"
            return {path[len(prefix):]: digest for path, digest in current.items() if path.startswith(prefix)}

        with (mock.patch.object(DELIVERY, "tree_hashes", side_effect=directory_hashes),
              mock.patch.object(DELIVERY, "checksum", return_value="f" * 64)):
            return delivery.source_match()

    def test_only_exact_reviewed_delivery_files_may_change_with_explicit_allowances(self):
        changes = {"scripts/deliver-generic-nominals.py": "b" * 64,
                   "scripts/test-deliver-generic-nominals.py": "c" * 64}
        result = self.compare(changes, list(changes))
        self.assertEqual(set(result["post_validation_changes"]), set(changes))
        self.assertTrue(result["committed_archive_matches_exact_snapshot_bytes"])

    def test_reviewed_helpers_still_require_explicit_changed_path_allowances(self):
        with self.assertRaisesRegex(RuntimeError, "unapproved post-validation"):
            self.compare({"scripts/deliver-generic-nominals.py": "b" * 64}, [])
        with self.assertRaisesRegex(RuntimeError, "unused post-validation allowance"):
            self.compare({}, ["scripts/deliver-generic-nominals.py"])

    def test_other_helper_compiler_and_language_test_changes_remain_rejected(self):
        for path in ("scripts/reproducibility.py", "scripts/test-package-toolchain.py",
                     "src/main.rs", "tests/generic_nominals.rs"):
            with self.subTest(path=path), self.assertRaisesRegex(RuntimeError, "two reviewed delivery helpers"):
                self.compare({path: "b" * 64}, [path])


if __name__ == "__main__":
    unittest.main()
