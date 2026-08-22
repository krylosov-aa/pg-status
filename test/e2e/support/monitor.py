"""Typed HTTP client and wait operations for the pg-status API."""

import json
from collections.abc import Mapping
from dataclasses import dataclass
from http import client as http_client
from types import MappingProxyType
from typing import cast
from urllib.parse import urlencode

from support.errors import E2EError
from support.waiting import Waiter

type Status = dict[str, object]
LSN_BASE = 16
LSN_SHIFT = 32


@dataclass(frozen=True)
class HTTPResult:
    """Fully-read HTTP response returned by the e2e client."""

    status: int
    body: str
    headers: Mapping[str, str]


class MonitorClient:
    """Read the pg-status HTTP interface over loopback."""

    def __init__(self, port: int) -> None:
        self._port = port

    def request(
        self,
        path: str,
        *,
        method: str = "GET",
        expected_status: int | None = 200,
        headers: Mapping[str, str] | None = None,
    ) -> tuple[int, str]:
        """Perform one HTTP request and optionally assert its status."""
        response_result = self.request_details(
            path,
            method=method,
            expected_status=expected_status,
            headers=headers,
        )
        return response_result.status, response_result.body

    def request_details(
        self,
        path: str,
        *,
        method: str = "GET",
        expected_status: int | None = 200,
        headers: Mapping[str, str] | None = None,
    ) -> HTTPResult:
        """Perform one HTTP request and preserve response headers."""
        connection = http_client.HTTPConnection(
            "127.0.0.1",
            self._port,
            timeout=5,
        )
        try:
            response_result = read_response(
                connection,
                path,
                method=method,
                headers=headers,
            )
        except http_client.HTTPException as error:
            raise E2EError(f"{method} {path} failed: {error}") from error
        finally:
            connection.close()

        if (
            expected_status is not None
            and response_result.status != expected_status
        ):
            raise E2EError(
                f"{method} {path} returned HTTP {response_result.status}, "
                f"expected {expected_status}; body={response_result.body!r}",
            )
        return response_result

    def text(self, path: str, expected_status: int = 200) -> str:
        """Return a text response and enforce its HTTP status."""
        _, body = self.request(path, expected_status=expected_status)
        return body

    def json(
        self,
        path: str,
        *,
        expected_status: int = 200,
        headers: Mapping[str, str] | None = None,
    ) -> object:
        """Decode one JSON response."""
        _, body = self.request(
            path,
            headers=headers,
            expected_status=expected_status,
        )
        try:
            return cast("object", json.loads(body))
        except json.JSONDecodeError as error:
            raise E2EError(
                f"GET {path} returned invalid JSON: {body!r}",
            ) from error

    def status(self, host: str) -> Status:
        """Return a validated host-status mapping."""
        query = urlencode({"host": host})
        payload = self.json(f"/status?{query}")
        if not isinstance(payload, dict):
            raise E2EError(
                f"unexpected status response for {host}: {payload!r}",
            )
        return cast("Status", payload)

    def hosts(self) -> dict[str, Status]:
        """Return statuses indexed by their monitor host names."""
        payload = self.json("/hosts")
        if not isinstance(payload, list):
            raise E2EError(f"unexpected hosts response: {payload!r}")
        return index_hosts(payload)


class MonitorWaiter:
    """Wait for pg-status to observe database state changes."""

    def __init__(self, client: MonitorClient, waiter: Waiter) -> None:
        self._client = client
        self._waiter = waiter

    def status(self, host: str, expected: Mapping[str, bool]) -> Status:
        """Wait until all requested status fields match."""
        return self._waiter.until(
            f"{host} status {dict(expected)}",
            lambda: self._client.status(host),
            lambda status: all(
                status.get(key) is expected_value
                for key, expected_value in expected.items()
            ),
        )

    def lsn(self, host: str, minimum_lsn: str) -> Status:
        """Wait until a host reports at least the requested WAL position."""
        minimum = parse_lsn(minimum_lsn)
        return self._waiter.until(
            f"{host} to replay {minimum_lsn}",
            lambda: self._client.status(host),
            lambda status: (
                status.get("alive") is True
                and isinstance(status.get("lsn"), str)
                and parse_lsn(cast("str", status["lsn"])) >= minimum
            ),
        )


def parse_lsn(location: str) -> int:
    """Convert PostgreSQL's hexadecimal WAL location to an integer."""
    try:
        high, low = location.split("/", 1)
    except ValueError as error:
        raise E2EError(f"invalid PostgreSQL LSN: {location!r}") from error
    try:
        return (int(high, LSN_BASE) << LSN_SHIFT) + int(low, LSN_BASE)
    except ValueError as error:
        raise E2EError(f"invalid PostgreSQL LSN: {location!r}") from error


def read_response(
    connection: http_client.HTTPConnection,
    path: str,
    *,
    method: str = "GET",
    headers: Mapping[str, str] | None = None,
) -> HTTPResult:
    """Perform one request and fully read its response."""
    request_headers = {} if headers is None else dict(headers)
    connection.request(
        method,
        path,
        headers=request_headers,
    )
    response = connection.getresponse()
    response_headers = MappingProxyType(
        {
            header_name.lower(): header_value
            for header_name, header_value in response.getheaders()
        }
    )
    return HTTPResult(
        status=response.status,
        body=response.read().decode("utf-8"),
        headers=response_headers,
    )


def index_hosts(payload: list[object]) -> dict[str, Status]:
    """Validate and index a hosts response."""
    hosts: dict[str, Status] = {}
    for host_status in payload:
        if not isinstance(host_status, dict):
            raise E2EError(f"unexpected host item: {host_status!r}")
        host = host_status.get("host")
        if not isinstance(host, str):
            raise E2EError(f"host item has no name: {host_status!r}")
        hosts[host] = cast("Status", host_status)
    return hosts
