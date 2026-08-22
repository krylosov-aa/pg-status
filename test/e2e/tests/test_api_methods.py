"""HTTP method coverage for the read-only pg-status API."""

import pytest

from support import api_helpers as helpers
from support.config import PROXIES
from support.monitor import MonitorClient


@pytest.mark.parametrize(
    "endpoint",
    (
        helpers.PATH_HOSTS,
        helpers.PATH_VERSION,
        helpers.PATH_MASTER,
        helpers.PATH_REPLICA,
        f"{helpers.PATH_STATUS}?host={PROXIES[0]}",
        helpers.PATH_SYNC_TIME,
        helpers.PATH_SYNC_BYTES,
        helpers.PATH_SYNC_TIME_OR_BYTES,
        helpers.PATH_SYNC_TIME_AND_BYTES,
        helpers.PATH_MOST_SYNC_BYTES,
    ),
)
def test_write_methods_are_rejected(
    monitor: MonitorClient,
    endpoint: str,
) -> None:
    """All public endpoints reject POST requests."""
    # Arrange
    method = "POST"

    # Act
    _, body = monitor.request(
        endpoint,
        method=method,
        expected_status=helpers.HTTP_BAD_METHOD,
    )

    # Assert
    assert body == ""


@pytest.mark.parametrize(
    "method",
    ("HEAD", "PUT", "DELETE", "OPTIONS", "TRACE", "PATCH"),
)
def test_all_non_get_method_kinds_are_rejected(
    monitor: MonitorClient,
    method: str,
) -> None:
    """The HTTP server exposes a strictly read-only public API."""
    # Arrange
    endpoint = helpers.PATH_VERSION

    # Act
    status, body = monitor.request(
        endpoint,
        method=method,
        expected_status=None,
    )

    # Assert
    assert status == helpers.HTTP_BAD_METHOD
    assert body == ""
