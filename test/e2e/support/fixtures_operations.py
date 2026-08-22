"""Pytest fixtures for stateful end-to-end test operations."""

from collections.abc import Iterator, Sequence
from typing import cast

import pytest

from support.faults import FaultController
from support.monitor import MonitorClient, MonitorWaiter
from support.postgres import PostgreSQL
from support.proxy import ProxyTopology
from support.waiting import Waiter
from support.wal import WalWriter


@pytest.fixture(scope="session")
def observed(monitor: MonitorClient, waiter: Waiter) -> MonitorWaiter:
    """Provide waits for state observed through pg-status."""
    return MonitorWaiter(monitor, waiter)


@pytest.fixture
def isolated_observed(
    isolated_monitor: MonitorClient,
    isolated_waiter: Waiter,
) -> MonitorWaiter:
    """Provide waits for state observed through isolated pg-status."""
    return MonitorWaiter(isolated_monitor, isolated_waiter)


@pytest.fixture
def faults(
    postgres: PostgreSQL,
    proxy: ProxyTopology,
) -> Iterator[FaultController]:
    """Apply recoverable faults and always restore them after one test."""
    controller = FaultController(postgres, proxy)
    try:
        yield controller
    finally:
        controller.restore()


@pytest.fixture(scope="session")
def wal(postgres: PostgreSQL) -> WalWriter:
    """Provide deterministic WAL-producing database operations."""
    return WalWriter(postgres)


_POSTGRES_SERVICES: Sequence[str] = (
    "primary",
    "replica-1",
    "replica-2",
)
_REPLICA_SERVICES: Sequence[str] = _POSTGRES_SERVICES[1:]


@pytest.fixture(autouse=True)
def reset_topology(
    request: pytest.FixtureRequest,
) -> Iterator[None]:
    """Reset topology to a stable baseline before and after each test."""
    if _uses_isolated_fixtures(request.fixturenames):
        proxy = request.getfixturevalue("isolated_proxy")
        postgres = request.getfixturevalue("isolated_postgres")
    else:
        proxy = request.getfixturevalue("proxy")
        postgres = request.getfixturevalue("postgres")

    _reset_topology(
        cast("ProxyTopology", proxy),
        cast("PostgreSQL", postgres),
    )
    try:
        yield
    finally:
        _reset_topology(
            cast("ProxyTopology", proxy),
            cast("PostgreSQL", postgres),
        )


def _uses_isolated_fixtures(fixturenames: Sequence[str]) -> bool:
    return any(
        fixture_name.startswith("isolated_") for fixture_name in fixturenames
    )


def _reset_topology(proxy: ProxyTopology, postgres: PostgreSQL) -> None:
    for service in _POSTGRES_SERVICES:
        postgres.set_paused(service, paused=False)
    proxy.reset()
    for replica in _REPLICA_SERVICES:
        if postgres.is_replica(replica):
            postgres.set_replay_paused(replica, paused=False)
