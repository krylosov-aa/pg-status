"""Selection behavior under PostgreSQL WAL replay lag."""

import time
from collections.abc import Callable

from support.faults import FaultController
from support.monitor import MonitorClient, MonitorWaiter, parse_lsn
from support.postgres import PostgreSQL
from support.proxy import ProxyTopology
from support.waiting import Waiter
from support.wal import WalWriter

PRIMARY = "primary"
FIRST_REPLICA = "replica-1"
SECOND_REPLICA = "replica-2"
FIRST_REPLICA_PROXY = "pg-proxy-2"
SECOND_REPLICA_PROXY = "pg-proxy-3"
PRIMARY_PROXY = "pg-proxy-1"
# Three times pg_status__sync_max_lag_ms in the e2e topology.
IDLE_COMMIT_AGE_MS = 1500
COMMIT_AGE_SQL = (
    "select (extract(epoch from now() - pg_last_xact_replay_timestamp())"
    " * 1000)::bigint"
)
# Timeline of the current WAL, as a client reads it after a write.
TIMELINE_SQL = (
    "select ('x' || substr(pg_walfile_name(pg_current_wal_insert_lsn()),"
    " 1, 8))::bit(32)::bigint"
)
# Observations one pg_status__sleep_ms apart, over several polling periods.
WINDOW_SAMPLES = 5
WINDOW_INTERVAL_SECONDS = 0.2

Selection = tuple[tuple[dict[str, object], ...], str]
IdleObservation = tuple[tuple[int, ...], tuple[Selection, ...]]


def test_minimum_lsn_filters_lagging_replica(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
    waiter: Waiter,
    wal: WalWriter,
) -> None:
    """Exclude a byte-lagging replica and recover its replay."""
    # Arrange
    warmup_lsn = wal.warm_up()
    observed.lsn(FIRST_REPLICA_PROXY, warmup_lsn)
    observed.lsn(SECOND_REPLICA_PROXY, warmup_lsn)
    faults.pause_replay(FIRST_REPLICA)

    # Act
    minimum_lsn = wal.generate_batch()
    lagging = waiter.until(
        "first replica byte lag",
        lambda: monitor.status(FIRST_REPLICA_PROXY),
        lambda status: numeric(status.get("lag_bytes")) > 0,
    )
    observed.lsn(SECOND_REPLICA_PROXY, minimum_lsn)
    selected = monitor.text(f"/replica?min_lsn={minimum_lsn}")
    faults.resume_replay(FIRST_REPLICA)
    recovered = observed.lsn(FIRST_REPLICA_PROXY, minimum_lsn)

    # Assert
    assert numeric(lagging.get("lag_bytes")) > 0
    assert selected == SECOND_REPLICA_PROXY
    assert parse_lsn(str(recovered["lsn"])) >= parse_lsn(minimum_lsn)


def test_primary_fallback_when_both_replicas_lag(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
    waiter: Waiter,
    wal: WalWriter,
) -> None:
    """Fall back to primary for both LSN and time-lag constraints."""
    # Arrange
    warmup_lsn = wal.warm_up()
    observed.lsn(FIRST_REPLICA_PROXY, warmup_lsn)
    observed.lsn(SECOND_REPLICA_PROXY, warmup_lsn)
    faults.pause_replay(FIRST_REPLICA)
    faults.pause_replay(SECOND_REPLICA)

    # Act
    minimum_lsn = wal.generate_batch()
    statuses = waiter.until(
        "both replicas behind generated WAL",
        lambda: replica_statuses(monitor),
        lambda observed: replicas_are_behind(observed, minimum_lsn),
    )
    timed_lag = waiter.until(
        "both replicas with time lag",
        lambda: replica_statuses(monitor),
        lambda observed: all(
            numeric(status.get("lag_ms")) > 0 for status in observed
        ),
    )
    selected = (
        monitor.text(f"/replica?min_lsn={minimum_lsn}"),
        monitor.text("/sync_by_time?lag_ms=0"),
    )

    # Assert
    assert replicas_are_behind(statuses, minimum_lsn)
    assert all(numeric(status.get("lag_ms")) > 0 for status in timed_lag)
    assert selected == (PRIMARY_PROXY, PRIMARY_PROXY)


def test_caught_up_replicas_stay_time_synchronous_without_commits(
    monitor: MonitorClient,
    observed: MonitorWaiter,
    postgres: PostgreSQL,
    waiter: Waiter,
    wal: WalWriter,
) -> None:
    """Bound the time lag of caught-up replicas while the primary is idle."""
    # Arrange
    warmup_lsn = wal.warm_up()
    observed.lsn(FIRST_REPLICA_PROXY, warmup_lsn)
    observed.lsn(SECOND_REPLICA_PROXY, warmup_lsn)

    # Act
    commit_ages, window = waiter.until(
        "replicas that stay time-synchronous long after the last commit",
        lambda: observe_idle_replicas(monitor, postgres),
        is_time_synchronous_while_idle,
    )

    # Assert
    assert min(commit_ages) > IDLE_COMMIT_AGE_MS
    for statuses, selected in window:
        assert all(numeric(status["lag_bytes"]) == 0 for status in statuses)
        assert all(status.get("sync_by_time") is True for status in statuses)
        assert selected in {FIRST_REPLICA_PROXY, SECOND_REPLICA_PROXY}


def test_caught_up_replicas_meet_the_write_timeline(
    monitor: MonitorClient,
    observed: MonitorWaiter,
    postgres: PostgreSQL,
    wal: WalWriter,
) -> None:
    """Route a read with the write's LSN and timeline to a replica."""
    # Arrange
    warmup_lsn = wal.warm_up()
    observed.lsn(FIRST_REPLICA_PROXY, warmup_lsn)
    observed.lsn(SECOND_REPLICA_PROXY, warmup_lsn)
    timeline = int(postgres.sql(PRIMARY, TIMELINE_SQL))

    # Act
    selected = monitor.text(
        f"/replica?min_lsn={warmup_lsn}&timeline={timeline}",
    )
    unknown_timeline = monitor.text(
        f"/replica?min_lsn={warmup_lsn}&timeline={timeline + 1}",
    )

    # Assert
    assert selected in {FIRST_REPLICA_PROXY, SECOND_REPLICA_PROXY}
    assert unknown_timeline == PRIMARY_PROXY


def test_replica_left_on_abandoned_history_is_excluded(
    isolated_monitor: MonitorClient,
    isolated_postgres: PostgreSQL,
    isolated_proxy: ProxyTopology,
    isolated_waiter: Waiter,
) -> None:
    """Exclude a replica that a failover left ahead of the new primary."""
    # Arrange: the replica to be promoted stops streaming and falls behind
    # the other one.
    isolated_proxy.reset()
    isolated_postgres.sql(
        FIRST_REPLICA,
        "alter system set primary_conninfo = ''",
    )
    isolated_postgres.sql(FIRST_REPLICA, "select pg_reload_conf()")
    isolated_waiter.until(
        "stopped WAL receiver of the replica to be promoted",
        lambda: isolated_postgres.sql(
            FIRST_REPLICA,
            "select count(*) from pg_stat_wal_receiver",
        ),
        lambda receivers: receivers == "0",
    )
    primary_lsn = WalWriter(isolated_postgres).generate_batch()
    isolated_waiter.until(
        "WAL catch-up of the replica to be abandoned",
        lambda: isolated_postgres.sql(
            SECOND_REPLICA,
            f"select pg_last_wal_replay_lsn() >= '{primary_lsn}'::pg_lsn",
        ),
        lambda caught_up: caught_up == "t",
    )
    isolated_postgres.stop_primary()
    isolated_postgres.promote(FIRST_REPLICA)
    isolated_postgres.sql(
        FIRST_REPLICA,
        "create table e2e_new_history (marker text)",
    )
    isolated_waiter.until(
        "master endpoint after promotion",
        lambda: isolated_monitor.text("/master"),
        lambda host: host == FIRST_REPLICA_PROXY,
    )

    # Act
    commit_ages, window = isolated_waiter.until(
        "abandoned replica long after its last commit",
        lambda: observe_abandoned_replica(isolated_monitor, isolated_postgres),
        lambda observation: observation[0][0] > IDLE_COMMIT_AGE_MS,
    )
    new_primary_lsn = isolated_postgres.sql(
        FIRST_REPLICA,
        "select pg_current_wal_insert_lsn()",
    )
    new_timeline = isolated_postgres.sql(FIRST_REPLICA, TIMELINE_SQL)
    abandoned_status = isolated_monitor.status(SECOND_REPLICA_PROXY)
    selected_by_bytes_or_lsn = tuple(
        isolated_monitor.text(route)
        for route in (
            "/sync_by_bytes",
            "/sync_by_time_or_bytes",
            "/most_sync_by_bytes",
            f"/replica?min_lsn={new_primary_lsn}",
            f"/replica?min_lsn={new_primary_lsn}&timeline={new_timeline}",
        )
    )
    selected_on_old_timeline = isolated_monitor.text(
        f"/replica?timeline={int(new_timeline) - 1}",
    )
    abandoned_state = isolated_postgres.sql(
        SECOND_REPLICA,
        "select concat_ws(',', pg_is_in_recovery(), "
        "to_regclass('e2e_new_history') is null, "
        f"pg_last_wal_replay_lsn() > '{new_primary_lsn}'::pg_lsn)",
    )

    # Assert
    assert commit_ages[0] > IDLE_COMMIT_AGE_MS
    # Still a replica, without the new primary's data but ahead of it.
    assert abandoned_state == "t,t,t"
    for statuses, selected in window:
        assert statuses[0].get("alive") is True
        assert statuses[0].get("sync_by_time") is False
        assert selected == FIRST_REPLICA_PROXY
    # Its position is on another timeline: no byte lag, no min_lsn.
    assert abandoned_status.get("lag_bytes") is None
    assert abandoned_status.get("sync_by_bytes") is False
    assert all(
        host == FIRST_REPLICA_PROXY for host in selected_by_bytes_or_lsn
    )
    # A write on the abandoned timeline vouches for no replica any more,
    # not even the one still on that timeline.
    assert selected_on_old_timeline == FIRST_REPLICA_PROXY


def observe_idle_replicas(
    monitor: MonitorClient,
    postgres: PostgreSQL,
) -> IdleObservation:
    """Read commit ages, then replica selections over several periods."""
    commit_ages = tuple(
        int(postgres.sql(replica, COMMIT_AGE_SQL))
        for replica in (FIRST_REPLICA, SECOND_REPLICA)
    )
    return commit_ages, observe_window(
        lambda: (replica_statuses(monitor), monitor.text("/sync_by_time")),
    )


def observe_abandoned_replica(
    monitor: MonitorClient,
    postgres: PostgreSQL,
) -> IdleObservation:
    """Read the commit age, then selections over several periods."""
    commit_age = int(postgres.sql(SECOND_REPLICA, COMMIT_AGE_SQL))
    return (commit_age,), observe_window(
        lambda: (
            (monitor.status(SECOND_REPLICA_PROXY),),
            monitor.text("/sync_by_time"),
        ),
    )


def observe_window(
    observe: Callable[[], Selection],
) -> tuple[Selection, ...]:
    """Repeat an observation once per polling period."""
    window = []
    for _ in range(WINDOW_SAMPLES):
        window.append(observe())
        time.sleep(WINDOW_INTERVAL_SECONDS)
    return tuple(window)


def is_time_synchronous_while_idle(observation: IdleObservation) -> bool:
    """Accept replicas time-synchronous throughout, long after a commit."""
    commit_ages, window = observation
    return min(commit_ages) > IDLE_COMMIT_AGE_MS and all(
        all(status.get("sync_by_time") is True for status in statuses)
        and selected != PRIMARY_PROXY
        for statuses, selected in window
    )


def replica_statuses(
    monitor: MonitorClient,
) -> tuple[dict[str, object], ...]:
    """Read statuses of both replica endpoints."""
    return (
        monitor.status(FIRST_REPLICA_PROXY),
        monitor.status(SECOND_REPLICA_PROXY),
    )


def replicas_are_behind(
    statuses: tuple[dict[str, object], ...],
    minimum_lsn: str,
) -> bool:
    """Return whether every status reports an older replay location."""
    minimum = parse_lsn(minimum_lsn)
    return all(replayed_before(status, minimum) for status in statuses)


def replayed_before(status: dict[str, object], minimum: int) -> bool:
    """Compare one reported replay location with an integer LSN."""
    replayed_lsn = parse_lsn(str(status["lsn"]))
    return replayed_lsn < minimum


def numeric(metric_value: object) -> float:
    """Require a JSON metric to be numeric."""
    if not isinstance(metric_value, int | float):
        raise AssertionError(
            f"expected numeric metric, got {metric_value!r}",
        )
    return float(metric_value)
