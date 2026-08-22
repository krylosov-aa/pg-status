"""Baseline pg-status behavior against one primary and two replicas."""

from support.config import POSTGRES_SERVICES, PROXIES
from support.monitor import MonitorClient
from support.postgres import PostgreSQL
from support.waiting import Waiter


def test_direct_postgresql_roles(postgres: PostgreSQL) -> None:
    """Read roles from PostgreSQL itself, without going through pg-status."""
    # Arrange
    expected = {"primary": "f", "replica-1": "t", "replica-2": "t"}

    # Act
    actual = {
        node: postgres.sql(node, "select pg_is_in_recovery()")
        for node in POSTGRES_SERVICES
    }

    # Assert
    assert actual == expected


def test_monitor_reports_default_topology(
    monitor: MonitorClient,
    waiter: Waiter,
) -> None:
    """Report every stable endpoint with its role and a real LSN."""
    # Arrange
    expected = {
        "pg-proxy-1": (True, True, True),
        "pg-proxy-2": (True, False, True),
        "pg-proxy-3": (True, False, True),
    }

    # Act
    hosts = waiter.until(
        "all proxy endpoints",
        monitor.hosts,
        lambda observed: set(observed) == set(PROXIES),
    )
    actual = {
        host: (
            status.get("alive"),
            status.get("master"),
            status.get("lsn") is not None,
        )
        for host, status in hosts.items()
    }

    # Assert
    assert actual == expected


def test_replica_round_robin(monitor: MonitorClient) -> None:
    """Return both eligible replicas over repeated requests."""
    # Arrange
    expected = {"pg-proxy-2", "pg-proxy-3"}

    # Act
    selected = {monitor.text("/replica") for unused in range(6)}

    # Assert
    assert selected == expected
