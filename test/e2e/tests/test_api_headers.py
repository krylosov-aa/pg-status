"""Response-header coverage for the public HTTP contract."""

from collections.abc import Mapping
from types import MappingProxyType

import pytest

from support import api_helpers as helpers
from support.config import PROXIES
from support.monitor import MonitorClient

ACCEPT_JSON = MappingProxyType({"Accept": "application/json"})
APPLICATION_JSON = "application/json"
TEXT_PLAIN = "text/plain; charset=utf-8"


@pytest.mark.parametrize(
    ("path", "request_headers", "expected_content_type"),
    (
        (helpers.PATH_VERSION, None, TEXT_PLAIN),
        (helpers.PATH_HOSTS, None, APPLICATION_JSON),
        (f"{helpers.PATH_STATUS}?host={PROXIES[0]}", None, APPLICATION_JSON),
        (helpers.PATH_MASTER, None, TEXT_PLAIN),
        (helpers.PATH_MASTER, ACCEPT_JSON, APPLICATION_JSON),
        (f"{helpers.PATH_REPLICA}?lag_ms=-1", None, APPLICATION_JSON),
    ),
)
def test_response_content_type(
    monitor: MonitorClient,
    path: str,
    request_headers: Mapping[str, str] | None,
    expected_content_type: str,
) -> None:
    """Every response body advertises its actual representation."""
    # Arrange
    content_type_header = "content-type"

    # Act
    response = monitor.request_details(
        path,
        headers=request_headers,
        expected_status=None,
    )

    # Assert
    assert response.headers[content_type_header] == expected_content_type


def test_method_not_allowed_advertises_get(monitor: MonitorClient) -> None:
    """A 405 response tells clients that GET is the allowed method."""
    # Arrange
    method = "POST"

    # Act
    response = monitor.request_details(
        helpers.PATH_VERSION,
        method=method,
        expected_status=helpers.HTTP_BAD_METHOD,
    )

    # Assert
    assert response.headers["allow"] == "GET"
    assert "content-type" not in response.headers


def test_empty_not_found_has_no_content_type(monitor: MonitorClient) -> None:
    """An empty 404 response does not claim to contain a representation."""
    # Arrange
    endpoint = helpers.PATH_UNKNOWN_ROUTE

    # Act
    response = monitor.request_details(
        endpoint,
        expected_status=helpers.HTTP_NOT_FOUND,
    )

    # Assert
    assert response.body == ""
    assert "content-type" not in response.headers
