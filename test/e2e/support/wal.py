"""Deterministic WAL-producing operations used by e2e tests."""

from support.postgres import PostgreSQL

PRIMARY = "primary"
WAL_INSERT_SQL = (
    "insert into e2e_wal(payload) "
    "select repeat(md5(value::text), 128) "
    "from generate_series(1, 4096) value"
)


class WalWriter:
    """Create timestamped transactions and sizeable WAL batches."""

    def __init__(self, postgres: PostgreSQL) -> None:
        self._postgres = postgres

    def warm_up(self) -> str:
        """Ensure replicas have replayed a transaction with a timestamp."""
        self._ensure_table()
        self._postgres.sql(
            PRIMARY,
            "insert into e2e_wal(payload) values ('warmup')",
        )
        return self._switch_and_read_lsn()

    def generate_batch(self) -> str:
        """Generate enough WAL for byte-lag observations."""
        self._ensure_table()
        self._postgres.sql(PRIMARY, WAL_INSERT_SQL)
        return self._switch_and_read_lsn()

    def _ensure_table(self) -> None:
        self._postgres.sql(
            PRIMARY,
            "create table if not exists e2e_wal ("
            "id bigint generated always as identity primary key, "
            "payload text not null)",
        )

    def _switch_and_read_lsn(self) -> str:
        self._postgres.sql(PRIMARY, "select pg_switch_wal()")
        return self._postgres.sql(PRIMARY, "select pg_current_wal_lsn()")
