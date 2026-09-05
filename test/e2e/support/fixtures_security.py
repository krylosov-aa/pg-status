"""Session fixture for PostgreSQL connection-security scenarios."""

from typing import Final

import pytest

from support.compose import ComposeProject
from support.security import SecurityTopology
from support.waiting import Waiter

_SESSION_SCOPE: Final = "session"


@pytest.fixture(scope=_SESSION_SCOPE)
def security(
    compose: ComposeProject,
    waiter: Waiter,
) -> SecurityTopology:
    """Provide the dedicated TLS and authentication topology."""
    topology = SecurityTopology(compose, waiter)
    topology.start()
    return topology
