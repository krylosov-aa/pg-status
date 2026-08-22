"""Lifecycle of the isolated PostgreSQL end-to-end environment."""

import logging
import os
from functools import partial
from pathlib import Path
from typing import Final

from support.compose import ComposeProject
from support.config import (
    INFRASTRUCTURE_SERVICES,
    POSTGRES_SERVICES,
    PROFILES,
    PROXIES,
    Profile,
)
from support.errors import E2EError
from support.monitor import MonitorClient
from support.postgres import PostgreSQL
from support.proxy import ProxyTopology
from support.waiting import Waiter

_MONITOR_SERVICE: Final = "pg-status"
_MONITOR_LOG_LINES: Final = 250
_MONITOR_STOP_TIMEOUT_SECONDS: Final = 30
_COMPOSE_DOWN_TIMEOUT_SECONDS: Final = 10


class E2EEnvironment:
    """Start and tear down one isolated Compose project."""

    def __init__(
        self,
        profile_name: str,
        project: str | None = None,
        *,
        monitor_service: str = _MONITOR_SERVICE,
    ) -> None:
        self._profile_name = profile_name
        self._monitor_service = monitor_service
        self._profile = find_profile(profile_name)
        self._project = project or default_project(profile_name)
        environment = profile_environment(self._profile, profile_name)
        self.compose = ComposeProject(
            _find_repository_root(),
            self._project,
            environment,
        )
        self.proxy = ProxyTopology(self.compose)
        self.postgres = PostgreSQL(self.compose)
        self.waiter = Waiter(self._profile.deadline_seconds)
        self._monitor: MonitorClient | None = None
        self._readiness = _EnvironmentReadiness(
            self.compose,
            self.proxy,
            self.postgres,
            self.waiter,
            monitor_service=self._monitor_service,
        )
        self._cleanup = _EnvironmentCleanup(
            self.compose,
            monitor_service=self._monitor_service,
        )

    @property
    def monitor(self) -> MonitorClient:
        """Expose the HTTP client after the environment has started."""
        if self._monitor is None:
            raise E2EError("e2e environment has not started")
        return self._monitor

    def start(self) -> None:
        """Start PostgreSQL, HAProxy proxies, and pg-status."""
        logger = logging.getLogger(__name__)
        logger.info(
            "Starting e2e profile=%s project=%s",
            self._profile_name,
            self._project,
        )
        self.compose.invoke(
            "up",
            "--detach",
            "--build",
            *INFRASTRUCTURE_SERVICES,
        )
        self._readiness.wait_for_proxies()
        self._readiness.wait_for_database_roles()
        self.proxy.reset()

        self.compose.invoke("build", self._monitor_service)
        self.compose.invoke(
            "up",
            "--detach",
            "--no-build",
            self._monitor_service,
        )
        self._monitor = MonitorClient(self._readiness.discover_port())
        self.waiter.until(
            "pg-status HTTP startup",
            lambda: self.monitor.text("/version"),
            lambda version: bool(version.strip()),
        )

    def close(self, tests_passed: bool) -> None:
        """Stop the monitor and remove all resources."""
        monitor_exit_code = self._cleanup.stop_monitor()
        if monitor_exit_code not in (None, 0):
            logger = logging.getLogger(__name__)
            logger.error(
                "pg-status diagnostics for %s:\n%s",
                self._project,
                self._cleanup.safe_logs(),
            )
        self._cleanup.unpause_databases()
        down_failed = self._cleanup.down()
        if tests_passed and (
            monitor_exit_code not in (None, 0) or down_failed
        ):
            details = []
            if monitor_exit_code not in (None, 0):
                details.append(f"pg-status exit code {monitor_exit_code}")
            if down_failed:
                details.append("compose down failed")
            raise E2EError(
                "environment teardown failed for project {}: {}".format(
                    self._project,
                    ", ".join(details),
                ),
            )


class _EnvironmentReadiness:
    """Wait for infrastructure and expose its dynamic HTTP port."""

    def __init__(
        self,
        compose: ComposeProject,
        proxy: ProxyTopology,
        postgres: PostgreSQL,
        waiter: Waiter,
        *,
        monitor_service: str,
    ) -> None:
        self._compose = compose
        self._proxy = proxy
        self._postgres = postgres
        self._waiter = waiter
        self._monitor_service = monitor_service

    def wait_for_proxies(self) -> None:
        """Wait for every HAProxy administrative socket."""
        for proxy in PROXIES:
            self._waiter.until(
                f"{proxy} HAProxy admin socket",
                partial(self._proxy.ready, proxy),
            )

    def wait_for_database_roles(self) -> None:
        """Wait for physical replication to expose the intended roles."""
        expected = {"primary": "f", "replica-1": "t", "replica-2": "t"}
        self._waiter.until(
            "direct PostgreSQL roles",
            self._read_roles,
            lambda roles: roles == expected,
        )

    def discover_port(self) -> int:
        """Read and validate the monitor's public port."""
        published = self._waiter.until(
            "published pg-status HTTP port",
            lambda: self._compose.invoke(
                "port",
                self._monitor_service,
                "8000",
                capture=True,
            ).stdout.strip(),
        )
        _, _, port = published.rpartition(":")
        if not port.isdigit():
            raise E2EError(
                f"cannot parse published pg-status port: {published}",
            )
        return int(port)

    def _read_roles(self) -> dict[str, str]:
        """Read roles directly from all PostgreSQL nodes."""
        statement = "select pg_is_in_recovery()"
        return {
            postgres: self._postgres.sql(postgres, statement)
            for postgres in POSTGRES_SERVICES
        }


class _EnvironmentCleanup:
    """Best-effort diagnostics and container cleanup operations."""

    def __init__(
        self,
        compose: ComposeProject,
        *,
        monitor_service: str,
    ) -> None:
        self._compose = compose
        self._monitor_service = monitor_service

    def stop_monitor(self) -> int | None:
        """Stop pg-status and convert errors into a non-zero exit code."""
        try:
            return self._stop_monitor()
        except (E2EError, ValueError) as error:
            logger = logging.getLogger(__name__)
            logger.error("Failed to stop pg-status cleanly: %s", error)
            return 1

    def down(self) -> bool:
        """Stop and remove compose resources, returning `True` on failure."""
        try:
            self._compose_down()
        except E2EError as error:
            logger = logging.getLogger(__name__)
            logger.error("compose down failed: %s", error)
            return True
        return False

    def safe_logs(self) -> str:
        """Return logs or a diagnostic explaining collection failure."""
        try:
            return self._compose.logs(
                self._monitor_service,
                tail=_MONITOR_LOG_LINES,
            )
        except E2EError as error:
            return f"could not collect logs: {error}"

    def unpause_databases(self) -> None:
        """Unfreeze databases before Compose removes the containers."""
        self._compose.invoke(
            "unpause",
            *POSTGRES_SERVICES.values(),
            check=False,
            capture=True,
        )

    def _compose_down(self) -> None:
        self._compose.invoke(
            "down",
            "--volumes",
            "--remove-orphans",
            "--timeout",
            str(_COMPOSE_DOWN_TIMEOUT_SECONDS),
        )

    def _stop_monitor(self) -> int | None:
        """Stop pg-status and return its last exit code."""
        container_id = self._compose.service_container_id(
            self._monitor_service,
        )
        if not container_id:
            return None
        self._compose.invoke(
            "stop",
            "--timeout",
            str(_MONITOR_STOP_TIMEOUT_SECONDS),
            self._monitor_service,
            check=False,
        )
        return self._compose.service_exit_code(self._monitor_service)


def find_profile(profile_name: str) -> Profile:
    """Resolve one named instrumentation profile."""
    profile = PROFILES.get(profile_name)
    if profile is None:
        choices = ", ".join(PROFILES)
        raise E2EError(
            f"unknown e2e profile {profile_name}; choose {choices}",
        )
    return profile


def default_project(profile_name: str) -> str:
    """Create an isolated per-process project name."""
    return f"pg-status-e2e-{profile_name}-{os.getpid()}"


def profile_environment(
    profile: Profile,
    profile_name: str,
) -> dict[str, str]:
    """Add selected build parameters to the inherited environment."""
    environment = os.environ.copy()
    environment.update(
        {
            "PG_STATUS_E2E_PROFILE": profile_name,
            "PG_STATUS_E2E_BUILD_TYPE": profile.build_type,
            "PG_STATUS_E2E_SANITIZER": profile.sanitizer,
            "PG_STATUS_E2E_MODE": profile.mode,
        },
    )
    return environment


def _find_repository_root() -> Path:
    """Locate repository root from nearby markers."""
    this_file = Path(__file__).resolve()
    marker = Path("test") / "docker" / "docker-compose.yml"
    for directory in (this_file, *this_file.parents):
        if (directory / marker).exists():
            return directory
    raise E2EError(
        "cannot locate repository root for pg-status e2e using marker "
        f"{marker!r}",
    )
