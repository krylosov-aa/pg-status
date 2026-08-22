"""Contract tests for stable API responses and data shapes."""

import re
from types import MappingProxyType
from typing import cast

from support import api_helpers as helpers
from support.config import PROXIES
from support.monitor import MonitorClient, index_hosts

ACCEPT_JSON_HEADER = MappingProxyType({"Accept": "application/json"})
ACCEPT_TEXT_HEADER = MappingProxyType({"Accept": "text/plain"})
ALIVE_FIELD = "alive"
BOOL_FIELDS = ("master", ALIVE_FIELD, "sync_by_time", "sync_by_bytes")
NULLABLE_NUMERIC_FIELDS = ("lag_ms", "lag_bytes")
NULLABLE_STRING_FIELDS = ("lsn",)


def _is_none_of_type(payload_value: object, value_type: type[object]) -> bool:
    return payload_value is None or isinstance(payload_value, value_type)


def _status_payload_is_valid(payload: dict[str, object]) -> bool:
    return (
        all(isinstance(payload[field], bool) for field in BOOL_FIELDS)
        and all(
            _is_none_of_type(payload[field], int)
            for field in NULLABLE_NUMERIC_FIELDS
        )
        and all(
            _is_none_of_type(payload[field], str)
            for field in NULLABLE_STRING_FIELDS
        )
    )


def test_version_contract(monitor: MonitorClient) -> None:
    """Version endpoint always returns text and ignores JSON header."""
    # Arrange
    semantic_version = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")

    # Act
    _, plain = monitor.request(
        helpers.PATH_VERSION, expected_status=helpers.HTTP_OK
    )
    _, json_variant = monitor.request(
        helpers.PATH_VERSION,
        headers=ACCEPT_JSON_HEADER,
        expected_status=helpers.HTTP_OK,
    )

    # Assert
    assert plain == json_variant
    assert semantic_version.fullmatch(plain) is not None


def test_hosts_contract(monitor: MonitorClient) -> None:
    """Hosts endpoint returns complete JSON payload in all cases."""
    # Arrange
    expected_count = len(PROXIES)

    # Act
    hosts = index_hosts(cast("list[object]", monitor.json(helpers.PATH_HOSTS)))
    text_accept_payload = cast(
        "list[object]",
        monitor.json(helpers.PATH_HOSTS, headers=ACCEPT_TEXT_HEADER),
    )

    # Assert
    assert set(hosts) == set(PROXIES)
    assert all(
        payload[helpers.HOST_FIELD] == host_name
        and {"lsn", "lag_ms", "lag_bytes", "host"}.issubset(payload)
        and _status_payload_is_valid(payload)
        for host_name, payload in hosts.items()
    )
    assert len(text_accept_payload) == expected_count


def test_host_selection_routes(monitor: MonitorClient) -> None:
    """Host-selection routes support both text and JSON output contracts."""
    # Arrange
    host_routes = (
        helpers.PATH_MASTER,
        helpers.PATH_REPLICA,
        helpers.PATH_SYNC_TIME,
        helpers.PATH_SYNC_BYTES,
        helpers.PATH_SYNC_TIME_OR_BYTES,
        helpers.PATH_SYNC_TIME_AND_BYTES,
        helpers.PATH_MOST_SYNC_BYTES,
    )

    # Act
    text_hosts = tuple(monitor.text(route) for route in host_routes)
    json_hosts = tuple(
        helpers.parse_json_object(
            monitor.request(route, headers=ACCEPT_JSON_HEADER)[1],
            route,
        )[helpers.HOST_FIELD]
        for route in host_routes
    )

    # Assert
    assert all(host in PROXIES for host in (*text_hosts, *json_hosts))


def test_master_is_text_and_json(monitor: MonitorClient) -> None:
    """Master endpoint always returns current master host."""
    # Arrange
    expected_hosts = set(PROXIES)

    # Act
    host = monitor.text(helpers.PATH_MASTER)
    payload = helpers.parse_json_object(
        monitor.request(
            helpers.PATH_MASTER,
            headers=ACCEPT_JSON_HEADER,
        )[1],
        helpers.PATH_MASTER,
    )

    # Assert
    assert host in expected_hosts
    assert payload == {helpers.HOST_FIELD: host}


def test_status_payload_types_for_all_hosts(monitor: MonitorClient) -> None:
    """All /status queries expose documented fields and types."""
    # Arrange
    sample_host = PROXIES[0]

    # Act
    sample_payload = cast(
        "dict[str, object]",
        monitor.json(f"{helpers.PATH_STATUS}?host={sample_host}"),
    )
    all_payloads_are_valid = all(
        _status_payload_is_valid(
            cast(
                "dict[str, object]",
                monitor.json(f"{helpers.PATH_STATUS}?host={host}"),
            )
        )
        for host in PROXIES
    )
    text_accept_payload = helpers.parse_json_object(
        monitor.request(
            f"{helpers.PATH_STATUS}?host={sample_host}",
            headers=ACCEPT_TEXT_HEADER,
        )[1],
        helpers.PATH_STATUS,
    )

    # Assert
    assert _status_payload_is_valid(sample_payload)
    assert all_payloads_are_valid
    assert sample_payload == text_accept_payload
