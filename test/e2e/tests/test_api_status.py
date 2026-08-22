"""Consistency coverage for per-host and aggregate status endpoints."""

from support.config import PROXIES
from support.monitor import MonitorClient

STABLE_STATUS_FIELDS = ("master", "alive", "lsn")


def test_status_matches_hosts_snapshot(monitor: MonitorClient) -> None:
    """Single-host status and all-hosts view expose the same host state."""
    # Arrange
    expected_hosts = set(PROXIES)

    # Act
    hosts = monitor.hosts()
    statuses = {host: monitor.status(host) for host in PROXIES}

    # Assert
    assert set(hosts) == expected_hosts
    assert all(
        all(
            statuses[host][field] == host_payload[field]
            for field in STABLE_STATUS_FIELDS
        )
        for host, host_payload in hosts.items()
    )
