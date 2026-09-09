"""Configuration and fixture plugins for explicit end-to-end tests."""

import signal
from types import FrameType

import pytest

from support.config import PROFILES

pytest_plugins = (
    "support.fixtures_infrastructure",
    "support.fixtures_isolated",
    "support.fixtures_operations",
    "support.fixtures_security",
)


def pytest_addoption(parser: pytest.Parser) -> None:
    """Register isolated Compose project and runtime profile options."""
    group = parser.getgroup("pg-status e2e")
    group.addoption(
        "--e2e-profile",
        choices=tuple(PROFILES),
        default="release",
        help="pg-status build and instrumentation profile",
    )
    group.addoption(
        "--e2e-project",
        help="explicit Docker Compose project name",
    )


def _interrupt(_signal: int, _frame: FrameType | None) -> None:
    raise KeyboardInterrupt("e2e interrupted; cleaning up owned resources")


def pytest_configure() -> None:
    """Let fixture finalizers run when the enclosing audit times out."""
    signal.signal(signal.SIGTERM, _interrupt)
