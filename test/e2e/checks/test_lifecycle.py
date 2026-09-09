"""Instrumented exits must fail even when all HTTP assertions passed."""

from pathlib import Path
from unittest.mock import Mock, patch

import pytest

from support.compose import ComposeProject
from support.errors import E2EError
from support.security import SecurityTopology


@pytest.mark.parametrize("exit_code", (99, 134, 137))
def test_monitor_exit_is_checked(exit_code: int, tmp_path: Path) -> None:
    compose = object.__new__(ComposeProject)
    compose._project = "isolated-test"
    compose._artifact_directory = str(tmp_path)
    with (
        patch.object(
            compose, "service_container_id", return_value="container"
        ),
        patch.object(compose, "invoke"),
        patch.object(compose, "service_exit_code", return_value=exit_code),
        patch.object(compose, "logs", return_value="final diagnostics"),
        pytest.raises(E2EError, match=str(exit_code)),
    ):
        compose.stop_monitor("pg-status-security-verify-full")
    assert (
        tmp_path / "isolated-test/pg-status-security-verify-full.log"
    ).read_text() == "final diagnostics"


def test_security_cleanup_validates_before_removing() -> None:
    compose = Mock()
    compose.invoke.side_effect = E2EError("startup failure")
    topology = SecurityTopology(compose, Mock())
    with pytest.raises(E2EError), topology.monitor("verify-full"):
        pytest.fail("startup unexpectedly succeeded")
    compose.stop_monitor.assert_called_once_with(
        "pg-status-security-verify-full"
    )
