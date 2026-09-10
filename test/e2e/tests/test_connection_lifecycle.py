"""Verify actual backend reuse, expiry and recovery, not just HTTP success."""

from support.monitor import MonitorWaiter
from support.postgres import PostgreSQL
from support.waiting import Waiter

_SESSION_SQL = (
    "select pid, query_start::text, "
    "extract(epoch from clock_timestamp()-backend_start) "
    "from pg_stat_activity where backend_type='client backend' "
    "and client_addr is not null and usename='postgres'"
)


def _session(postgres: PostgreSQL) -> tuple[str, str, float] | None:
    rows = postgres.sql("primary", _SESSION_SQL).splitlines()
    if len(rows) != 1:
        return None
    pid, query, age = rows[0].split("|")
    return pid, query, float(age)


def test_connection_reused_then_recycled(
    postgres: PostgreSQL,
    waiter: Waiter,
) -> None:
    # Arrange
    first = waiter.until(
        "fresh backend",
        lambda: _session(postgres),
        lambda row: row is not None and row[2] < 0.5,
    )
    assert first is not None

    # Act
    second = waiter.until(
        "another poll on the same backend",
        lambda: _session(postgres),
        lambda row: row is not None and row[1] != first[1],
    )

    # Assert
    assert second is not None
    assert second[0] == first[0], "a healthy connection was not reused"

    # Act
    replacement = waiter.until(
        "connection max age causes recycle",
        lambda: _session(postgres),
        lambda row: row is not None and row[0] != first[0],
        timeout=15,
    )

    # Assert
    assert replacement is not None


def test_server_closed_connection_recovers(
    postgres: PostgreSQL,
    waiter: Waiter,
    observed: MonitorWaiter,
) -> None:
    # Arrange
    before = waiter.until("backend exists", lambda: _session(postgres))
    assert before is not None

    # Act
    postgres.sql("primary", f"select pg_terminate_backend({int(before[0])})")

    # Assert
    waiter.until(
        "new backend after server termination",
        lambda: _session(postgres),
        lambda row: row is not None and row[0] != before[0],
    )
    observed.status("pg-proxy-1", {"alive": True, "master": True})
