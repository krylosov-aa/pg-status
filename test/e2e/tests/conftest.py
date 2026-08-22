"""Configuration and fixture plugins for explicit end-to-end tests."""

import pytest

from support.config import PROFILES

pytest_plugins = (
    "support.fixtures_infrastructure",
    "support.fixtures_isolated",
    "support.fixtures_operations",
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
