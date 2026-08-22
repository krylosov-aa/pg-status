"""Error-path coverage for pg-status HTTP handlers."""

import pytest

from support import api_helpers as helpers
from support.faults import FaultController
from support.monitor import MonitorClient
from support.waiting import Waiter

ALIVE_FIELD = "alive"
ERROR_MIN_LSN = "Invalid min_lsn"
SELECTION_ROUTES = (
    helpers.PATH_REPLICA,
    helpers.PATH_SYNC_TIME,
    helpers.PATH_SYNC_BYTES,
    helpers.PATH_SYNC_TIME_OR_BYTES,
    helpers.PATH_SYNC_TIME_AND_BYTES,
    helpers.PATH_MOST_SYNC_BYTES,
)
HOST_SELECTION_ROUTES = (helpers.PATH_MASTER, *SELECTION_ROUTES)


@pytest.mark.parametrize(
    ("query", "error_text"),
    (
        ("lag_ms=-1", "Invalid lag_ms"),
        ("lag_bytes=18446744073709551616", "Invalid lag_bytes"),
        ("min_lsn=invalid", ERROR_MIN_LSN),
    ),
)
@pytest.mark.parametrize("route", SELECTION_ROUTES)
def test_invalid_query_parameters(
    monitor: MonitorClient,
    route: str,
    query: str,
    error_text: str,
) -> None:
    """Every filtering endpoint rejects every invalid query field."""
    # Arrange
    path = f"{route}?{query}"

    # Act
    _, body = monitor.request(
        path,
        expected_status=helpers.HTTP_BAD_REQUEST,
    )

    # Assert
    assert helpers.parse_json_object(body, path) == {
        "error_text": error_text,
    }


@pytest.mark.parametrize(
    "query",
    (
        "lag_ms=",
        "lag_ms=abc",
        "lag_ms=1x",
        "lag_ms=18446744073709551616",
        "min_lsn=",
        "min_lsn=100",
        "min_lsn=G/1",
        "min_lsn=1/G",
        "min_lsn=1/2/3",
    ),
)
def test_malformed_query_parameter_forms(
    monitor: MonitorClient,
    query: str,
) -> None:
    """Reject empty, malformed, trailing, and overflowing values."""
    # Arrange
    path = f"{helpers.PATH_REPLICA}?{query}"

    # Act
    status, _ = monitor.request(path, expected_status=None)

    # Assert
    assert status == helpers.HTTP_BAD_REQUEST


def test_status_requires_host_param(monitor: MonitorClient) -> None:
    """status endpoint rejects missing host query parameter."""
    # Arrange
    endpoint = helpers.PATH_STATUS

    # Act
    _, body = monitor.request(
        endpoint,
        expected_status=helpers.HTTP_BAD_REQUEST,
    )

    # Assert
    assert body == f'{{"error_text": "{helpers.REQUEST_HOST_ERROR}"}}'


def test_unknown_route_returns_not_found(monitor: MonitorClient) -> None:
    """Unknown routes must return empty 404 text response."""
    # Arrange
    endpoint = helpers.PATH_UNKNOWN_ROUTE

    # Act
    _, body = monitor.request(
        endpoint,
        expected_status=helpers.HTTP_NOT_FOUND,
    )

    # Assert
    assert body == ""


def test_status_unknown_host_returns_not_found(
    monitor: MonitorClient,
) -> None:
    """Status should return 404 for unknown host names."""
    # Arrange
    unknown_host = helpers.PATH_UNKNOWN_ROUTE[1:]

    # Act
    _, body = monitor.request(
        f"{helpers.PATH_STATUS}?host={unknown_host}",
        expected_status=helpers.HTTP_NOT_FOUND,
    )

    # Assert
    assert body == ""


def test_unavailable_topology_returns_not_found(
    faults: FaultController,
    monitor: MonitorClient,
    waiter: Waiter,
) -> None:
    """Unavailable topology returns 404 and null host-selection JSON."""
    # Arrange
    faults.pause_postgres("primary")
    faults.pause_postgres("replica-1")
    faults.pause_postgres("replica-2")

    # Act
    waiter.until(
        "all topology nodes are unavailable",
        monitor.hosts,
        lambda hosts: all(
            isinstance(payload.get(ALIVE_FIELD), bool)
            and not bool(payload.get(ALIVE_FIELD))
            for payload in hosts.values()
        ),
    )

    # Assert
    for endpoint in HOST_SELECTION_ROUTES:
        _, body = monitor.request(
            endpoint,
            expected_status=helpers.HTTP_NOT_FOUND,
        )
        assert body == ""

    for endpoint in HOST_SELECTION_ROUTES:
        _, body = monitor.request(
            endpoint,
            headers={"Accept": "application/json"},
            expected_status=helpers.HTTP_NOT_FOUND,
        )
        assert helpers.parse_json_object(body, endpoint) == {
            helpers.HOST_FIELD: None,
        }

    assert monitor.status(helpers.PROXY_ONE)[ALIVE_FIELD] is False


def test_status_reports_dead_host_as_not_alive(
    faults: FaultController,
    monitor: MonitorClient,
    waiter: Waiter,
) -> None:
    """status should expose dead hosts with null metrics when paused."""
    # Arrange
    faults.pause_postgres("replica-1")

    # Act
    host_status = waiter.until(
        "replica host becomes unavailable",
        lambda: monitor.status(helpers.PROXY_ONE),
        lambda payload: payload.get(ALIVE_FIELD) is False,
    )

    # Assert
    assert all(
        key in host_status and host_status[key] is None
        for key in ("lag_ms", "lag_bytes", "lsn")
    )
