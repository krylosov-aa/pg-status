"""Regression checks for audit isolation, provenance, and bounded commands."""

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "audit_run", Path(__file__).parents[1] / "run.py"
)
audit_run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit_run)


class RunnerTests(unittest.TestCase):
    def test_timeout_terminates_child_processes(self):
        # Arrange
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "child-survived"
            child = (
                "import signal,time; from pathlib import Path; "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "time.sleep(1); "
                f"Path({str(marker)!r}).touch()"
            )
            command = [
                sys.executable,
                "-c",
                "import subprocess,sys,time; "
                f"subprocess.Popen([sys.executable, '-c', {child!r}]); "
                "time.sleep(30)",
            ]

            # Act & Assert
            with self.assertRaises(subprocess.TimeoutExpired):
                audit_run.run_command(
                    command, root, os.environ.copy(), root / "log", 0.1
                )
            import time

            time.sleep(1.1)

            # Assert
            self.assertFalse(marker.exists())

    def test_snapshot_preserves_local_edits_but_excludes_caches(self):
        # Arrange
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".gitignore").write_text("out/\n__pycache__/\n")
            source = root / "source.c"
            source.write_text("before")
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)
            source.write_text("local edit")
            (root / "__pycache__").mkdir()
            (root / "__pycache__/module.pyc").write_bytes(b"cache")
            destination = root / "out/source"

            # Act
            manifest = audit_run.source_snapshot(root, destination)

            # Assert
            self.assertEqual(
                (destination / "source.c").read_text(), "local edit"
            )
            self.assertNotIn("__pycache__/module.pyc", manifest)

            # Act
            source.write_text("later edit")

            # Assert
            self.assertEqual(
                (destination / "source.c").read_text(), "local edit"
            )

    def test_cleanup_filter_is_owned_by_run(self):
        # Arrange
        audit = object.__new__(audit_run.Audit)
        audit.label = "com.pg-status.audit.run=our-run"
        audit.run_id = "our-run"
        audit.images = []
        audit.report = {}
        commands = []
        audit.output = lambda command: commands.append(command) or ""
        audit.save = lambda: None

        # Act
        audit.cleanup()

        # Assert
        self.assertEqual(len(commands), 7)
        for command in commands:
            self.assertIn("label=com.pg-status.audit.run=our-run", command)

    def test_snapshot_excludes_its_own_custom_artifact_directory(self):
        # Arrange
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / "source.c").write_text("input")
            artifacts = root / "custom-results"
            artifacts.mkdir()
            (artifacts / "report.json").write_text("running")

            # Act
            manifest = audit_run.source_snapshot(root, artifacts / "source")

            # Assert
            self.assertEqual(set(manifest), {"source.c"})

    def test_user_image_alias_is_not_owned(self):
        # Arrange
        audit = object.__new__(audit_run.Audit)
        audit.run_id = "our-run"

        # Act & Assert
        self.assertTrue(audit.owns_e2e_tag("pg-status-e2e:asan-our-run"))
        self.assertFalse(audit.owns_e2e_tag("pg-status-e2e:asan-other-run"))
        self.assertFalse(audit.owns_e2e_tag("my-retained-image:latest"))


if __name__ == "__main__":
    unittest.main()
