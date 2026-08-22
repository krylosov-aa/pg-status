"""Session-scoped fixtures for shared end-to-end test infrastructure."""

from collections.abc import Iterator
from typing import Final

import pytest

from support.compose import ComposeProject
from support.environment import E2EEnvironment
from support.fixtures_builder import build_environment
from support.monitor import MonitorClient
from support.postgres import PostgreSQL
from support.proxy import ProxyTopology
from support.waiting import Waiter

_SESSION_SCOPE: Final = "session"


@pytest.fixture(scope=_SESSION_SCOPE)
def environment(request: pytest.FixtureRequest) -> Iterator[E2EEnvironment]:
    """Own the lifecycle of one real, isolated topology."""
    e2e_environment: E2EEnvironment = build_environment(
        request,
        isolated=False,
    )
    e2e_environment.start()
    try:
        yield e2e_environment
    finally:
        e2e_environment.close(request.session.testsfailed == 0)


@pytest.fixture(scope=_SESSION_SCOPE)
def compose(environment: E2EEnvironment) -> ComposeProject:
    """Provide access to the active shared Compose project."""
    return environment.compose


@pytest.fixture(scope=_SESSION_SCOPE)
def proxy(environment: E2EEnvironment) -> ProxyTopology:
    """Provide stable endpoint routing controls."""
    return environment.proxy


@pytest.fixture(scope=_SESSION_SCOPE)
def postgres(environment: E2EEnvironment) -> PostgreSQL:
    """Provide direct access to the real PostgreSQL nodes."""
    return environment.postgres


@pytest.fixture(scope=_SESSION_SCOPE)
def monitor(environment: E2EEnvironment) -> MonitorClient:
    """Provide the pg-status HTTP client."""
    return environment.monitor


@pytest.fixture(scope=_SESSION_SCOPE)
def waiter(environment: E2EEnvironment) -> Waiter:
    """Provide deadline-based retries for eventual consistency."""
    return environment.waiter
