"""PostgreSQL role changes behind stable HAProxy endpoint names."""

from support.faults import FaultController
from support.monitor import MonitorClient, MonitorWaiter
from support.waiting import Waiter

PRIMARY_PROXY = "pg-proxy-1"
PROMOTED_PROXY = "pg-proxy-2"
ALIVE_FIELD = "alive"
MASTER_FIELD = "master"


def test_proxy_role_switch(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
    waiter: Waiter,
) -> None:
    """Observe a primary and replica swap without changing endpoint names."""
    # Arrange
    expected = {
        PRIMARY_PROXY: (True, False),
        PROMOTED_PROXY: (True, True),
    }

    # Act
    faults.route(PRIMARY_PROXY, "replica-1")
    faults.route(PROMOTED_PROXY, "primary")
    first = observed.status(
        PRIMARY_PROXY,
        {ALIVE_FIELD: True, MASTER_FIELD: False},
    )
    second = observed.status(
        PROMOTED_PROXY,
        {ALIVE_FIELD: True, MASTER_FIELD: True},
    )
    selected_master = waiter.until(
        "switched master endpoint",
        lambda: monitor.text("/master"),
        lambda host: host == PROMOTED_PROXY,
    )
    actual = {
        PRIMARY_PROXY: (first.get(ALIVE_FIELD), first.get(MASTER_FIELD)),
        PROMOTED_PROXY: (second.get(ALIVE_FIELD), second.get(MASTER_FIELD)),
    }

    # Assert
    assert actual == expected
    assert selected_master == PROMOTED_PROXY
