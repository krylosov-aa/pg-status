"""Shared helpers for pg-status API e2e coverage."""

import json
from typing import cast

from support.monitor import parse_lsn

HTTP_BAD_METHOD = 405
HTTP_BAD_REQUEST = 400
HTTP_NOT_FOUND = 404
HTTP_OK = 200
UINT64_MAX = 18_446_744_073_709_551_615
LAG_BYTES_ZERO = 0
LAG_MS_ZERO = 0
REPLICA_ONE = "replica-1"
REPLICA_TWO = "replica-2"
PROXY_MASTER = "pg-proxy-1"
PROXY_ONE = "pg-proxy-2"
PROXY_TWO = "pg-proxy-3"
PATH_UNKNOWN_ROUTE = "/definitely-missing"
PATH_VERSION = "/version"
PATH_STATUS = "/status"
PATH_HOSTS = "/hosts"
PATH_MASTER = "/master"
PATH_REPLICA = "/replica"
PATH_SYNC_TIME = "/sync_by_time"
PATH_SYNC_BYTES = "/sync_by_bytes"
PATH_SYNC_TIME_OR_BYTES = "/sync_by_time_or_bytes"
PATH_SYNC_TIME_AND_BYTES = "/sync_by_time_and_bytes"
PATH_MOST_SYNC_BYTES = "/most_sync_by_bytes"
REQUEST_HOST_ERROR = "Get parameter 'host' wasn't passed"
HOST_FIELD = "host"


def parse_json_object(raw_body: str, path: str) -> dict[str, object]:
    """Parse a JSON object and validate its type."""
    payload = _parse_json(raw_body, path)
    if not isinstance(payload, dict):
        raise AssertionError(f"expected object for {path}, got {payload!r}")
    return cast("dict[str, object]", payload)


def is_behind(status: dict[str, object], threshold: str) -> bool:
    """Return whether replica lag is behind target threshold."""
    raw_lsn = status.get("lsn")
    if not isinstance(raw_lsn, str):
        return False
    return parse_lsn(raw_lsn) < parse_lsn(threshold)


def metric(status: dict[str, object], field: str) -> int:
    """Return one required integer metric from a status payload."""
    metric_value = status.get(field)
    if not isinstance(metric_value, int):
        raise AssertionError(
            f"expected integer {field}, got {metric_value!r}",
        )
    return metric_value


def _parse_json(raw_body: str, path: str) -> object:
    """Parse raw JSON and add contextual assertion text."""
    try:
        return cast("object", json.loads(raw_body))
    except json.JSONDecodeError as error:
        raise AssertionError(
            f"unable to decode JSON from {path}: {raw_body!r}",
        ) from error
