"""Function-scoped fixtures for per-test isolated end-to-end topologies."""

from collections.abc import Iterator

import pytest

from support.environment import E2EEnvironment
from support.fixtures_builder import build_environment
from support.monitor import MonitorClient
from support.postgres import PostgreSQL
from support.proxy import ProxyTopology
from support.waiting import Waiter


@pytest.fixture
def isolated_environment(
    request: pytest.FixtureRequest,
) -> Iterator[E2EEnvironment]:
    """Own the lifecycle of one isolated topology for a single test."""
    e2e_environment: E2EEnvironment = build_environment(
        request,
        isolated=True,
    )
    e2e_environment.start()
    try:
        yield e2e_environment
    finally:
        e2e_environment.close(request.session.testsfailed == 0)


@pytest.fixture
def isolated_proxy(isolated_environment: E2EEnvironment) -> ProxyTopology:
    """Provide stable endpoint routing controls in isolated topology."""
    return isolated_environment.proxy


@pytest.fixture
def isolated_postgres(isolated_environment: E2EEnvironment) -> PostgreSQL:
    """Provide direct access to the real PostgreSQL nodes."""
    return isolated_environment.postgres


@pytest.fixture
def isolated_monitor(isolated_environment: E2EEnvironment) -> MonitorClient:
    """Provide the pg-status HTTP client in isolated topology."""
    return isolated_environment.monitor


@pytest.fixture
def isolated_waiter(isolated_environment: E2EEnvironment) -> Waiter:
    """Provide deadline-based retries in isolated topology."""
    return isolated_environment.waiter
