"""Connection and query faults against real PostgreSQL and HAProxy."""

from collections.abc import Callable

from support.errors import E2EError
from support.postgres import PostgreSQL
from support.proxy import ProxyTopology


def _restore_many(
    subjects: tuple[str, ...],
    restore: Callable[[str], None],
) -> list[E2EError]:
    """Try to rollback all affected items and return collected errors."""
    errors: list[E2EError] = []
    for subject in subjects:
        try:
            restore(subject)
        except E2EError as error:
            errors.append(error)
    return errors


class FaultController:
    """Apply faults and automatically rollback them at fixture teardown."""

    def __init__(self, postgres: PostgreSQL, proxy: ProxyTopology) -> None:
        self._postgres = postgres
        self._proxy = proxy
        self._paused_replay: set[str] = set()
        self._paused_postgres: set[str] = set()
        self._routes_changed = False

    def route(self, proxy: str, target: str | None) -> None:
        """Route one proxy to a specific backend or disconnect it."""
        self._proxy.route(proxy, target)
        self._routes_changed = True

    def pause_replay(self, replica: str) -> None:
        """Pause WAL replay and remember to resume it later."""
        self._postgres.set_replay_paused(replica, paused=True)
        self._paused_replay.add(replica)

    def resume_replay(self, replica: str) -> None:
        """Resume WAL replay and forget this replica from the rollback plan."""
        self._postgres.set_replay_paused(replica, paused=False)
        self._paused_replay.discard(replica)

    def pause_postgres(self, postgres: str) -> None:
        """Pause one PostgreSQL container and remember to resume it."""
        self._postgres.set_paused(postgres, paused=True)
        self._paused_postgres.add(postgres)

    def resume_postgres(self, postgres: str) -> None:
        """Resume one PostgreSQL container and forget this node."""
        self._postgres.set_paused(postgres, paused=False)
        self._paused_postgres.discard(postgres)

    def restore(self) -> None:
        """Rollback all active faults in a best-effort order."""
        errors: list[E2EError] = []
        errors.extend(
            _restore_many(
                tuple(self._paused_postgres),
                self.resume_postgres,
            )
        )
        errors.extend(
            _restore_many(
                tuple(self._paused_replay),
                self.resume_replay,
            )
        )
        if self._routes_changed:
            try:
                self._proxy.reset()
            except E2EError as error:
                errors.append(error)
        if errors:
            raise E2EError(f"failed to restore faults: {errors}")
