"""Primary failover in isolated topology."""

from support.monitor import MonitorClient, MonitorWaiter
from support.postgres import PostgreSQL
from support.proxy import ProxyTopology
from support.waiting import Waiter

PRIMARY = "primary"
PROMOTED_REPLICA = "replica-1"
PRIMARY_PROXY = "pg-proxy-1"
PROMOTED_PROXY = "pg-proxy-2"
ALIVE_FIELD = "alive"
MASTER_FIELD = "master"


def test_real_postgresql_failover(
    isolated_monitor: MonitorClient,
    isolated_observed: MonitorWaiter,
    isolated_postgres: PostgreSQL,
    isolated_proxy: ProxyTopology,
    isolated_waiter: Waiter,
) -> None:
    """Promote a caught-up replica after stopping the real primary."""
    # Arrange
    isolated_proxy.reset()
    isolated_postgres.sql(
        PRIMARY,
        "create table e2e_failover (marker text primary key)",
    )
    isolated_postgres.sql(
        PRIMARY,
        "insert into e2e_failover values ('survives-failover')",
    )
    isolated_postgres.sql(PRIMARY, "select pg_switch_wal()")
    primary_lsn = isolated_postgres.sql(
        PRIMARY,
        "select pg_current_wal_lsn()",
    )
    isolated_waiter.until(
        "promotion candidate WAL catch-up",
        lambda: isolated_postgres.sql(
            PROMOTED_REPLICA,
            f"select pg_last_wal_replay_lsn() >= '{primary_lsn}'::pg_lsn",
        ),
        lambda caught_up: caught_up == "t",
    )

    # Act
    isolated_postgres.stop_primary()
    isolated_postgres.promote(PROMOTED_REPLICA)
    old_primary = isolated_observed.status(
        PRIMARY_PROXY,
        {ALIVE_FIELD: False, MASTER_FIELD: False},
    )
    promoted = isolated_observed.status(
        PROMOTED_PROXY,
        {ALIVE_FIELD: True, MASTER_FIELD: True},
    )
    selected_master = isolated_waiter.until(
        "master endpoint after promotion",
        lambda: isolated_monitor.text("/master"),
        lambda host: host == PROMOTED_PROXY,
    )
    database_state = (
        isolated_postgres.sql(
            PROMOTED_REPLICA,
            "select pg_is_in_recovery()",
        ),
        isolated_postgres.sql(
            PROMOTED_REPLICA,
            "select marker from e2e_failover",
        ),
    )

    # Assert
    assert (old_primary.get(ALIVE_FIELD), old_primary.get(MASTER_FIELD)) == (
        False,
        False,
    )
    assert (promoted.get(ALIVE_FIELD), promoted.get(MASTER_FIELD)) == (
        True,
        True,
    )
    assert selected_master == PROMOTED_PROXY
    assert database_state == ("f", "survives-failover")
