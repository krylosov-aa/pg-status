"""Host-selection behavior when topology members are unavailable."""

from types import MappingProxyType

from support import api_helpers as helpers
from support.faults import FaultController
from support.monitor import MonitorClient, MonitorWaiter

ACCEPT_JSON = MappingProxyType({"Accept": "application/json"})


def test_replicas_remain_usable_without_a_master(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
) -> None:
    """A missing master does not make healthy replicas unavailable."""
    # Arrange
    faults.pause_postgres("primary")
    observed.status(
        helpers.PROXY_MASTER,
        {"alive": False, "master": False},
    )

    # Act
    master_response = monitor.request(
        helpers.PATH_MASTER,
        expected_status=None,
    )
    json_response = monitor.request(
        helpers.PATH_MASTER,
        headers=ACCEPT_JSON,
        expected_status=None,
    )
    selected_replica = monitor.text(helpers.PATH_REPLICA)
    filtered_response = monitor.request(
        f"{helpers.PATH_REPLICA}?min_lsn=ffffffff/ffffffff",
        expected_status=None,
    )

    # Assert
    assert master_response == (helpers.HTTP_NOT_FOUND, "")
    assert json_response[0] == helpers.HTTP_NOT_FOUND
    assert helpers.parse_json_object(
        json_response[1], helpers.PATH_MASTER
    ) == {
        helpers.HOST_FIELD: None,
    }
    assert selected_replica in {helpers.PROXY_ONE, helpers.PROXY_TWO}
    assert filtered_response == (helpers.HTTP_NOT_FOUND, "")


def test_all_selection_routes_skip_a_dead_replica(
    faults: FaultController,
    monitor: MonitorClient,
    observed: MonitorWaiter,
) -> None:
    """Every selection policy ignores a PostgreSQL node that is down."""
    # Arrange
    faults.pause_postgres(helpers.REPLICA_ONE)
    observed.status(helpers.PROXY_ONE, {"alive": False})
    routes = (
        helpers.PATH_REPLICA,
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
        (f"{helpers.PATH_MOST_SYNC_BYTES}?lag_bytes={helpers.UINT64_MAX}"),
    )

    # Act
    selected = tuple(monitor.text(route) for route in routes)

    # Assert
    assert selected == (helpers.PROXY_TWO,) * len(routes)
