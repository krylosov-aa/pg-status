"""Sync-route coverage for time, bytes, and combined selection policies."""

import pytest

from support import api_helpers as helpers
from support.config import PROXIES
from support.faults import FaultController
from support.monitor import MonitorClient, MonitorWaiter
from support.waiting import Waiter
from support.wal import WalWriter

LAG_MS_FIELD = "lag_ms"
LAG_BYTES_FIELD = "lag_bytes"


def test_minimum_lsn_applies_to_every_sync_route(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
    waiter: Waiter,
    wal: WalWriter,
) -> None:
    """Every sync route excludes a replica behind the requested LSN."""
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
    sync_paths = (
        (
            f"{helpers.PATH_SYNC_TIME}"
            f"?min_lsn={minimum_lsn}&lag_ms={helpers.UINT64_MAX}"
        ),
        (
            f"{helpers.PATH_SYNC_BYTES}"
            f"?min_lsn={minimum_lsn}&lag_bytes={helpers.UINT64_MAX}"
        ),
        (
            f"{helpers.PATH_SYNC_TIME_OR_BYTES}"
            f"?min_lsn={minimum_lsn}&lag_ms={helpers.UINT64_MAX}"
        ),
        (
            f"{helpers.PATH_SYNC_TIME_AND_BYTES}"
            f"?min_lsn={minimum_lsn}&lag_ms={helpers.UINT64_MAX}"
            f"&lag_bytes={helpers.UINT64_MAX}"
        ),
        (
            f"{helpers.PATH_MOST_SYNC_BYTES}"
            f"?min_lsn={minimum_lsn}&lag_bytes={helpers.UINT64_MAX}"
        ),
    )

    # Act
    selected = tuple(monitor.text(path) for path in sync_paths)

    # Assert
    assert selected == (helpers.PROXY_TWO,) * len(sync_paths)


def test_lag_dimensions_and_boolean_policies(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
    waiter: Waiter,
    wal: WalWriter,
) -> None:
    """Apply time, byte, OR, AND, and most-sync policies independently."""
    # Arrange
    warmup_lsn = wal.warm_up()
    observed.lsn(helpers.PROXY_ONE, warmup_lsn)
    observed.lsn(helpers.PROXY_TWO, warmup_lsn)
    faults.pause_replay(helpers.REPLICA_ONE)
    minimum_lsn = wal.generate_batch()
    waiter.until(
        "first replica has measurable time and byte lag",
        lambda: monitor.status(helpers.PROXY_ONE),
        lambda status: (
            helpers.metric(status, LAG_MS_FIELD) > 0
            and helpers.metric(status, LAG_BYTES_FIELD) > 0
            and helpers.is_behind(status, minimum_lsn)
        ),
    )
    observed.lsn(helpers.PROXY_TWO, minimum_lsn)
    faults.pause_postgres(helpers.REPLICA_TWO)
    observed.status(helpers.PROXY_TWO, {"alive": False})
    time_passes = f"lag_ms={helpers.UINT64_MAX}&lag_bytes=0"
    bytes_pass = f"lag_ms=0&lag_bytes={helpers.UINT64_MAX}"

    # Act
    selected = {
        "time": monitor.text(f"{helpers.PATH_SYNC_TIME}?{time_passes}"),
        "bytes": monitor.text(f"{helpers.PATH_SYNC_BYTES}?{bytes_pass}"),
        "or_time": monitor.text(
            f"{helpers.PATH_SYNC_TIME_OR_BYTES}?{time_passes}"
        ),
        "or_bytes": monitor.text(
            f"{helpers.PATH_SYNC_TIME_OR_BYTES}?{bytes_pass}"
        ),
        "and_time": monitor.text(
            f"{helpers.PATH_SYNC_TIME_AND_BYTES}?{time_passes}"
        ),
        "and_bytes": monitor.text(
            f"{helpers.PATH_SYNC_TIME_AND_BYTES}?{bytes_pass}"
        ),
        "most_sync": monitor.text(
            f"{helpers.PATH_MOST_SYNC_BYTES}?{bytes_pass}"
        ),
    }

    # Assert
    assert selected == {
        "time": helpers.PROXY_ONE,
        "bytes": helpers.PROXY_ONE,
        "or_time": helpers.PROXY_ONE,
        "or_bytes": helpers.PROXY_ONE,
        "and_time": helpers.PROXY_MASTER,
        "and_bytes": helpers.PROXY_MASTER,
        "most_sync": helpers.PROXY_ONE,
    }


def test_most_sync_by_bytes_selects_smallest_lag(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
    waiter: Waiter,
    wal: WalWriter,
) -> None:
    """most_sync_by_bytes chooses the least byte-lagging live replica."""
    # Arrange
    warmup_lsn = wal.warm_up()
    observed.lsn(helpers.PROXY_ONE, warmup_lsn)
    observed.lsn(helpers.PROXY_TWO, warmup_lsn)
    faults.pause_replay(helpers.REPLICA_ONE)
    minimum_lsn = wal.generate_batch()
    observed.lsn(helpers.PROXY_TWO, minimum_lsn)
    lag_pair = waiter.until(
        "replicas have distinct byte lag",
        lambda: (
            monitor.status(helpers.PROXY_ONE),
            monitor.status(helpers.PROXY_TWO),
        ),
        lambda statuses: (
            helpers.metric(statuses[0], LAG_BYTES_FIELD)
            > helpers.metric(statuses[1], LAG_BYTES_FIELD)
        ),
    )

    # Act
    selected = monitor.text(
        f"{helpers.PATH_MOST_SYNC_BYTES}?lag_bytes={helpers.UINT64_MAX}"
    )

    # Assert
    assert helpers.metric(lag_pair[0], LAG_BYTES_FIELD) > helpers.metric(
        lag_pair[1], LAG_BYTES_FIELD
    )
    assert selected == helpers.PROXY_TWO


def test_most_sync_by_bytes_falls_back_to_master(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
    waiter: Waiter,
    wal: WalWriter,
) -> None:
    """most_sync_by_bytes falls back when no replica is fresh enough."""
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

    # Act
    selected = monitor.text(
        f"{helpers.PATH_MOST_SYNC_BYTES}?min_lsn={minimum_lsn}"
        f"&lag_bytes={helpers.UINT64_MAX}"
    )

    # Assert
    assert selected == helpers.PROXY_MASTER


def test_most_sync_tie_uses_host_order(
    monitor: MonitorClient,
    observed: MonitorWaiter,
    waiter: Waiter,
    wal: WalWriter,
) -> None:
    """Equal byte lag is resolved deterministically by configured order."""
    # Arrange
    minimum_lsn = wal.warm_up()
    observed.lsn(helpers.PROXY_ONE, minimum_lsn)
    observed.lsn(helpers.PROXY_TWO, minimum_lsn)
    waiter.until(
        "both replicas have zero byte lag",
        lambda: (
            monitor.status(helpers.PROXY_ONE),
            monitor.status(helpers.PROXY_TWO),
        ),
        lambda statuses: all(
            helpers.metric(status, LAG_BYTES_FIELD) == 0 for status in statuses
        ),
    )

    # Act
    selected = tuple(
        monitor.text(
            f"{helpers.PATH_MOST_SYNC_BYTES}?lag_bytes={helpers.UINT64_MAX}"
        )
        for unused in range(3)
    )

    # Assert
    assert selected == (helpers.PROXY_ONE,) * len(selected)


@pytest.mark.parametrize(
    "path",
    (
        f"{helpers.PATH_SYNC_TIME}?lag_ms={helpers.UINT64_MAX}",
        f"{helpers.PATH_SYNC_BYTES}?lag_bytes={helpers.UINT64_MAX}",
        (
            f"{helpers.PATH_SYNC_TIME_OR_BYTES}"
            f"?lag_ms={helpers.UINT64_MAX}&lag_bytes={helpers.UINT64_MAX}"
        ),
        (
            f"{helpers.PATH_SYNC_TIME_AND_BYTES}"
            f"?lag_ms={helpers.UINT64_MAX}&lag_bytes={helpers.UINT64_MAX}"
        ),
    ),
)
def test_sync_routes_use_round_robin(
    monitor: MonitorClient,
    path: str,
) -> None:
    """Every round-robin sync route returns both eligible replicas."""
    # Arrange
    expected = {helpers.PROXY_ONE, helpers.PROXY_TWO}

    # Act
    selected = {monitor.text(path) for unused in range(6)}

    # Assert
    assert selected == expected


def test_sync_routes_accept_default_query_values(
    monitor: MonitorClient,
) -> None:
    """Sync routes accept omitted lag and min_lsn parameters."""
    # Arrange
    default_paths = (
        helpers.PATH_SYNC_TIME,
        helpers.PATH_SYNC_BYTES,
        helpers.PATH_SYNC_TIME_OR_BYTES,
        helpers.PATH_SYNC_TIME_AND_BYTES,
        helpers.PATH_MOST_SYNC_BYTES,
    )

    # Act
    selected = tuple(monitor.text(path) for path in default_paths)

    # Assert
    assert all(host in PROXIES for host in selected)
