"""Runtime HAProxy routing controls."""

from support.compose import ComposeProject
from support.config import BACKENDS, PROXIES
from support.errors import E2EError


class ProxyTopology:
    """Route stable monitor endpoints to selected PostgreSQL nodes."""

    def __init__(self, compose: ComposeProject) -> None:
        self._compose = compose

    def command(self, proxy: str, command: str) -> None:
        """Send one command to the HAProxy administrative socket."""
        if proxy not in PROXIES:
            raise E2EError(f"unknown proxy: {proxy}")
        self._compose.invoke(
            "exec",
            "-T",
            proxy,
            "socat",
            "stdio",
            "/run/haproxy/admin.sock",
            capture=True,
            stdin=f"{command}\n",
        )

    def route(self, proxy: str, target: str | None) -> None:
        """Enable one backend and close sessions to every other backend."""
        selected = selected_backend(target)
        if selected is not None:
            self.command(proxy, f"enable server pg_backends/{selected}")
        disabled_backends = set(BACKENDS.values()).difference({selected})
        for backend in disabled_backends:
            self.command(proxy, f"disable server pg_backends/{backend}")
            self.command(
                proxy,
                f"shutdown sessions server pg_backends/{backend}",
            )

    def reset(self) -> None:
        """Restore the production-like primary and replica routes."""
        self.route("pg-proxy-1", "primary")
        self.route("pg-proxy-2", "replica-1")
        self.route("pg-proxy-3", "replica-2")

    def ready(self, proxy: str) -> bool:
        """Return true after the administrative socket accepts a command."""
        self.command(proxy, "show info")
        return True


def selected_backend(target: str | None) -> str | None:
    """Resolve a route target while accepting an explicit disconnection."""
    if target is None:
        return None
    try:
        return BACKENDS[target]
    except KeyError as error:
        raise E2EError(f"unknown PostgreSQL backend: {target}") from error
