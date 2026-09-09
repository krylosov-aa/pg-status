"""Fault injection for the e2e runner, without PostgreSQL or Docker."""

import os
import sys
from pathlib import Path

import pytest

from support.errors import E2EError
from support.process import CommandRunner


def test_timeout_is_a_failure(tmp_path: Path) -> None:
    runner = CommandRunner(tmp_path, os.environ)
    with pytest.raises(E2EError, match="deadline"):
        runner.run(
            (sys.executable, "-c", "import time; time.sleep(60)"), timeout=0.05
        )


def test_command_failure_preserves_diagnostics(tmp_path: Path) -> None:
    runner = CommandRunner(tmp_path, os.environ)
    with pytest.raises(E2EError, match="injected error"):
        runner.run(
            (
                sys.executable,
                "-c",
                "import sys; print('injected error'); sys.exit(99)",
            ),
            capture=True,
        )
