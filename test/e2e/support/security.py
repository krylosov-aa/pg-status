"""TLS and authentication controls for connection-security tests."""

from collections.abc import Iterator
from contextlib import contextmanager
from functools import partial

from support.compose import ComposeProject
from support.errors import E2EError
from support.monitor import MonitorClient
from support.waiting import Waiter

SECURITY_DATABASE = "postgres-security"
SECURITY_MONITOR_PREFIX = "pg-status-security-"
ADMIN_ROLE = "postgres"
ADMIN_DATABASE = "postgres"


class SecurityTopology:
    """Operate the isolated TLS database and short-lived monitors."""

    def __init__(
        self,
        compose: ComposeProject,
        waiter: Waiter,
    ) -> None:
        self._compose = compose
        self._waiter = waiter

    def start(self) -> None:
        """Build and start the dedicated TLS PostgreSQL service."""
        self._compose.invoke(
            "up",
            "--detach",
            "--build",
            SECURITY_DATABASE,
        )
        self._waiter.until(
            "security PostgreSQL readiness",
            partial(_database_ready, self._compose),
        )

    @contextmanager
    def monitor(self, scenario: str) -> Iterator[MonitorClient]:
        """Run one configured monitor scenario and remove it afterwards."""
        service = _monitor_service(scenario)
        self._compose.invoke("up", "--detach", "--no-build", service)
        monitor = MonitorClient(
            _discover_port(self._compose, self._waiter, service),
        )
        self._waiter.until(
            f"{service} HTTP startup",
            lambda: monitor.text("/version"),
            lambda version: bool(version.strip()),
        )
        try:
            yield monitor
        finally:
            self._compose.invoke(
                "rm",
                "--stop",
                "--force",
                service,
                capture=True,
                check=False,
            )

    def logs(self, scenario: str) -> str:
        """Return logs for an active security monitor."""
        return self._compose.logs(_monitor_service(scenario))

    def sql(self, statement: str) -> str:
        """Run an administrative query on the security database."""
        command_result = self._compose.invoke(
            "exec",
            "-T",
            SECURITY_DATABASE,
            "psql",
            "-v",
            "ON_ERROR_STOP=1",
            "-U",
            ADMIN_ROLE,
            "-d",
            ADMIN_DATABASE,
            "-At",
            "-c",
            statement,
            capture=True,
        )
        return command_result.stdout.strip()

    def connection_uses_tls(self, user: str) -> bool:
        """Report TLS state for a monitor's current database session."""
        statement = (
            "select a.usename, bool_and(s.ssl) "
            "from pg_stat_activity a "
            "join pg_stat_ssl s using (pid) "
            "where a.client_addr is not null "
            "group by a.usename"
        )
        states = dict(
            row.split("|", 1)
            for row in self.sql(statement).splitlines()
            if "|" in row
        )
        return states.get(user) == "t"


def _database_ready(compose: ComposeProject) -> bool:
    command_result = compose.invoke(
        "exec",
        "-T",
        SECURITY_DATABASE,
        "psql",
        "-U",
        ADMIN_ROLE,
        "-d",
        ADMIN_DATABASE,
        "-At",
        "-c",
        "select 1",
        capture=True,
        check=False,
    )
    return command_result.return_code == 0


def _discover_port(
    compose: ComposeProject,
    waiter: Waiter,
    service: str,
) -> int:
    published = waiter.until(
        f"published {service} HTTP port",
        lambda: compose.invoke(
            "port",
            service,
            "8000",
            capture=True,
        ).stdout.strip(),
    )
    _, _, port = published.rpartition(":")
    if not port.isdigit():
        raise E2EError(
            f"cannot parse published {service} port: {published}",
        )
    return int(port)


def _monitor_service(scenario: str) -> str:
    if not scenario or not scenario.replace("-", "").isalnum():
        raise E2EError(f"invalid security scenario: {scenario!r}")
    return f"{SECURITY_MONITOR_PREFIX}{scenario}"
