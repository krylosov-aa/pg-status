"""Direct PostgreSQL controls used to create real database states."""

from support.compose import ComposeProject
from support.config import POSTGRES_SERVICES
from support.errors import E2EError


class PostgreSQL:
    """Operate PostgreSQL nodes through their Compose containers."""

    def __init__(self, compose: ComposeProject) -> None:
        self._compose = compose

    def sql(self, postgres: str, statement: str) -> str:
        """Execute SQL directly on a named PostgreSQL node."""
        service = postgres_service(postgres)
        command_result = self._compose.invoke(
            "exec",
            "-T",
            service,
            "psql",
            "-v",
            "ON_ERROR_STOP=1",
            "-U",
            "postgres",
            "-d",
            "postgres",
            "-At",
            "-c",
            statement,
            capture=True,
        )
        return command_result.stdout.strip()

    def set_replay_paused(self, replica: str, *, paused: bool) -> None:
        """Pause or resume WAL replay on one replica."""
        operation = "pause" if paused else "resume"
        self.sql(replica, f"select pg_wal_replay_{operation}()")

    def is_replica(self, postgres: str) -> bool:
        """Return whether a node is currently a replica."""
        return self.sql(postgres, "select pg_is_in_recovery()") == "t"

    def set_paused(
        self,
        postgres: str,
        *,
        paused: bool,
    ) -> None:
        """Idempotently freeze or unfreeze a PostgreSQL container."""
        service = postgres_service(postgres)
        if self._compose.service_is_paused(service) is paused:
            return
        operation = "pause" if paused else "unpause"
        self._compose.invoke(
            operation,
            service,
        )

    def stop_primary(self) -> None:
        """Stop the original primary database."""
        self._compose.invoke("stop", "--timeout", "5", "postgres-primary")

    def promote(self, replica: str) -> None:
        """Promote one replica and require PostgreSQL to confirm it."""
        promoted = self.sql(replica, "select pg_promote(true, 30)")
        if promoted != "t":
            raise E2EError(
                f"PostgreSQL did not promote {replica}: {promoted}",
            )


def postgres_service(postgres: str) -> str:
    """Resolve a public node name to its Compose service."""
    service = POSTGRES_SERVICES.get(postgres)
    if service is None:
        raise E2EError(f"unknown PostgreSQL service: {postgres}")
    return service
