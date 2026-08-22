"""Connection and query failures observed by pg-status."""

from datetime import UTC, datetime, timedelta
from types import MappingProxyType

from support.compose import ComposeProject
from support.faults import FaultController
from support.monitor import MonitorWaiter
from support.waiting import Waiter

FAULT_PROXY = "pg-proxy-3"
ALIVE_FIELD = "alive"
MASTER_FIELD = "master"
EXPECTED_REPLICA = MappingProxyType(
    {ALIVE_FIELD: True, MASTER_FIELD: False},
)
EXPECTED_DEAD = MappingProxyType(
    {ALIVE_FIELD: False, MASTER_FIELD: False},
)


def test_disconnect_and_recovery(
    compose: ComposeProject,
    faults: FaultController,
    observed: MonitorWaiter,
    waiter: Waiter,
) -> None:
    """Expose possible_dead diagnostics and recover after reconnection."""
    # Arrange
    marker = "host=pg-proxy-3 state=possible_dead"
    marker_since = datetime.now(UTC) - timedelta(seconds=1)

    # Act
    faults.route(FAULT_PROXY, None)
    dead = observed.status(FAULT_PROXY, EXPECTED_DEAD)
    waiter.until(
        "disconnect marker in pg-status logs",
        lambda: compose.logs(
            "pg-status",
            since=marker_since,
        ),
        lambda logs: marker in logs,
    )
    faults.route(FAULT_PROXY, "replica-2")
    recovered = observed.status(FAULT_PROXY, EXPECTED_REPLICA)

    # Assert
    assert (dead.get(ALIVE_FIELD), dead.get(MASTER_FIELD)) == (False, False)
    assert (recovered.get(ALIVE_FIELD), recovered.get(MASTER_FIELD)) == (
        True,
        False,
    )


def test_query_timeout_and_recovery(
    compose: ComposeProject,
    faults: FaultController,
    observed: MonitorWaiter,
    waiter: Waiter,
) -> None:
    """Enforce the query deadline while a PostgreSQL process is frozen."""
    # Arrange
    marker = "PostgreSQL poll timed out host=pg-proxy-3"
    marker_since = datetime.now(UTC) - timedelta(seconds=1)

    # Act
    faults.pause_postgres("replica-2")
    timed_out = observed.status(FAULT_PROXY, EXPECTED_DEAD)
    waiter.until(
        "query-timeout marker in pg-status logs",
        lambda: compose.logs(
            "pg-status",
            since=marker_since,
        ),
        lambda logs: marker in logs,
    )
    faults.resume_postgres("replica-2")
    recovered = observed.status(FAULT_PROXY, EXPECTED_REPLICA)

    # Assert
    assert (timed_out.get(ALIVE_FIELD), timed_out.get(MASTER_FIELD)) == (
        False,
        False,
    )
    assert (recovered.get(ALIVE_FIELD), recovered.get(MASTER_FIELD)) == (
        True,
        False,
    )
