"""Selection behavior under PostgreSQL WAL replay lag."""

from support.faults import FaultController
from support.monitor import MonitorClient, MonitorWaiter, parse_lsn
from support.waiting import Waiter
from support.wal import WalWriter

FIRST_REPLICA = "replica-1"
SECOND_REPLICA = "replica-2"
FIRST_REPLICA_PROXY = "pg-proxy-2"
SECOND_REPLICA_PROXY = "pg-proxy-3"
PRIMARY_PROXY = "pg-proxy-1"


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
