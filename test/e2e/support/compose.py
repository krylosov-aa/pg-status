"""Docker Compose access for an isolated end-to-end project."""

import shutil
from collections.abc import Mapping
from datetime import UTC, datetime
from pathlib import Path

from support.errors import E2EError
from support.process import CommandResult, CommandRunner


def find_compose_command() -> tuple[str, ...]:
    """Find Docker Compose without invoking a command shell."""
    docker = shutil.which("docker")
    if docker is not None:
        probe = CommandRunner(Path.cwd(), {}).run(
            (docker, "compose", "version"),
            capture=True,
            check=False,
        )
        if probe.return_code == 0:
            return (docker, "compose")
    docker_compose = shutil.which("docker-compose")
    if docker_compose is not None:
        return (docker_compose,)
    raise E2EError("Docker Compose plugin or docker-compose is required")


class ComposeProject:
    """Commands scoped to one uniquely named Compose project."""

    def __init__(
        self,
        repository_root: Path,
        project: str,
        environment: Mapping[str, str],
    ) -> None:
        self._runner = CommandRunner(repository_root, environment)
        compose_file = repository_root / "test/docker/docker-compose.yml"
        self._prefix = (
            *find_compose_command(),
            "--project-name",
            project,
            "--file",
            str(compose_file),
            "--profile",
            "pg-status",
            "--profile",
            "security",
        )

    def invoke(
        self,
        *arguments: str,
        capture: bool = False,
        check: bool = True,
        stdin: str | None = None,
    ) -> CommandResult:
        """Invoke Docker Compose inside this project."""
        return self._runner.run(
            (*self._prefix, *arguments),
            capture=capture,
            check=check,
            stdin=stdin,
        )

    def service_container_id(self, service: str) -> str:
        """Return the current container identifier for a service."""
        command_result = self.invoke(
            "ps",
            "--all",
            "--quiet",
            service,
            capture=True,
        )
        return command_result.stdout.strip()

    def service_exit_code(self, service: str) -> int:
        """Inspect the saved exit status of a service container."""
        container_id = self.service_container_id(service)
        if not container_id:
            raise E2EError(f"container does not exist for service: {service}")
        docker = shutil.which("docker")
        if docker is None:
            raise E2EError("docker executable is required")
        command_result = self._runner.run(
            (
                docker,
                "inspect",
                "--format",
                "{{.State.ExitCode}}",
                container_id,
            ),
            capture=True,
        )
        return int(command_result.stdout.strip())

    def service_is_paused(self, service: str) -> bool:
        """Return Docker's paused state for one Compose service."""
        container_id = self.service_container_id(service)
        if not container_id:
            raise E2EError(f"container does not exist for service: {service}")
        docker = shutil.which("docker")
        if docker is None:
            raise E2EError("docker executable is required")
        command_result = self._runner.run(
            (
                docker,
                "inspect",
                "--format",
                "{{.State.Paused}}",
                container_id,
            ),
            capture=True,
        )
        paused = command_result.stdout.strip()
        if paused not in {"true", "false"}:
            raise E2EError(
                f"unexpected paused state for {service}: {paused!r}",
            )
        return paused == "true"

    def logs(
        self,
        service: str | None = None,
        *,
        tail: int | None = None,
        since: str | datetime | None = None,
    ) -> str:
        """Return filtered logs from one service or from all services."""
        arguments = ["logs", "--no-color"]
        if tail is not None:
            arguments.extend(["--tail", str(tail)])
        if since is not None:
            if isinstance(since, datetime):
                arguments.extend(["--since", _format_timestamp(since)])
            else:
                arguments.extend(["--since", since])
        if service is not None:
            arguments.append(service)
        return self.invoke(*arguments, capture=True).stdout


def _format_timestamp(timestamp: datetime) -> str:
    """Format a timestamp accepted by compose logs."""
    return timestamp.astimezone(UTC).replace(microsecond=0).isoformat()
