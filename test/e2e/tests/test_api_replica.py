"""Replica-selection coverage for /replica endpoint and liveness filters."""

from support import api_helpers as helpers
from support.faults import FaultController
from support.monitor import MonitorClient, MonitorWaiter
from support.waiting import Waiter
from support.wal import WalWriter


def test_replica_filtering_falls_back_to_master(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
    waiter: Waiter,
    wal: WalWriter,
) -> None:
    """Filter constraints can force /replica to return the master."""
    # Arrange
    warmup_lsn = wal.warm_up()
    observed.lsn(helpers.PROXY_ONE, warmup_lsn)
    observed.lsn(helpers.PROXY_TWO, warmup_lsn)
    faults.pause_replay(helpers.REPLICA_ONE)
    faults.pause_replay(helpers.REPLICA_TWO)
    minimum_lsn = wal.generate_batch()
    waiter.until(
        "both replicas behind minimum lsn",
        lambda: (
            monitor.status(helpers.PROXY_ONE),
            monitor.status(helpers.PROXY_TWO),
        ),
        lambda statuses: all(
            helpers.is_behind(status, minimum_lsn) for status in statuses
        ),
    )
    failing_queries = (
        f"lag_ms={helpers.LAG_MS_ZERO}&min_lsn={minimum_lsn}",
        f"lag_bytes={helpers.LAG_BYTES_ZERO}&min_lsn={minimum_lsn}",
        (
            f"lag_ms={helpers.LAG_MS_ZERO}"
            f"&lag_bytes={helpers.LAG_BYTES_ZERO}&min_lsn={minimum_lsn}"
        ),
    )

    # Act
    selected = tuple(
        monitor.text(f"{helpers.PATH_REPLICA}?{query}")
        for query in failing_queries
    )

    # Assert
    assert selected == (helpers.PROXY_MASTER,) * len(failing_queries)


def test_replica_min_lsn_boundary_is_inclusive(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
    waiter: Waiter,
    wal: WalWriter,
) -> None:
    """/replica should match exact min_lsn and fallback above it."""
    # Arrange
    warmup_lsn = wal.warm_up()
    observed.lsn(helpers.PROXY_ONE, warmup_lsn)
    observed.lsn(helpers.PROXY_TWO, warmup_lsn)
    faults.pause_replay(helpers.REPLICA_ONE)
    minimum_lsn = wal.generate_batch()
    waiter.until(
        "first replica behind minimum lsn",
        lambda: monitor.status(helpers.PROXY_ONE),
        lambda status: helpers.is_behind(status, minimum_lsn),
    )
    observed.lsn(helpers.PROXY_TWO, minimum_lsn)

    # Act
    exact_boundary = monitor.text(
        f"{helpers.PATH_REPLICA}?min_lsn={minimum_lsn}"
    )
    impossible_boundary = monitor.text(
        f"{helpers.PATH_REPLICA}?min_lsn=ffffffff/ffffffff"
    )

    # Assert
    assert exact_boundary == helpers.PROXY_TWO
    assert impossible_boundary == helpers.PROXY_MASTER
