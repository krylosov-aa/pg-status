"""Exercise receiver-statistics fallback against a real restricted replica."""

from support.postgres import PostgreSQL
from support.security import SecurityTopology
from support.waiting import Waiter

_PERMISSION_SQL = (
    "select has_table_privilege('fallback_monitor', "
    "'pg_catalog.pg_stat_wal_receiver', 'select')"
)


def test_receiver_permissions_recover_after_reconnection(
    postgres: PostgreSQL,
    security: SecurityTopology,
    waiter: Waiter,
) -> None:
    postgres.sql(
        "primary", "create role fallback_monitor login password 'fallback'"
    )
    try:
        postgres.sql(
            "primary",
            "revoke select on pg_catalog.pg_stat_wal_receiver from public",
        )
        waiter.until(
            "replica applies role and ACL",
            lambda: postgres.sql("replica-1", _PERMISSION_SQL),
            lambda result: result == "f",
        )
        with security.monitor("restricted") as monitor:
            waiter.until(
                "restricted replica stays alive",
                lambda: monitor.status("postgres-replica-1"),
                lambda status: (
                    status["alive"] is True and status["master"] is False
                ),
            )
            waiter.until(
                "fallback warning",
                lambda: security.logs("restricted"),
                lambda logs: (
                    "retrying poll without WAL receiver statistics" in logs
                ),
            )
            postgres.sql(
                "primary",
                "grant select on pg_catalog.pg_stat_wal_receiver "
                "to fallback_monitor",
            )
            waiter.until(
                "replica applies grant",
                lambda: postgres.sql("replica-1", _PERMISSION_SQL),
                lambda result: result == "t",
            )
            # Force reconnection and assert the full query is actually used,
            # rather than merely observing an unchanged alive snapshot.
            postgres.sql(
                "replica-1",
                "select pg_terminate_backend(pid) from pg_stat_activity "
                "where usename='fallback_monitor'",
            )
            waiter.until(
                "full receiver query after reconnect",
                lambda: postgres.sql(
                    "replica-1",
                    "select query from pg_stat_activity "
                    "where usename='fallback_monitor'",
                ),
                lambda query: "from pg_catalog.pg_stat_wal_receiver" in query,
            )
            assert monitor.status("postgres-replica-1")["alive"] is True
    finally:
        postgres.sql(
            "primary",
            "grant select on pg_catalog.pg_stat_wal_receiver to public",
        )
        postgres.sql("primary", "drop owned by fallback_monitor")
        postgres.sql("primary", "drop role fallback_monitor")
