"""Fixed configuration for the Docker end-to-end matrix."""

from dataclasses import dataclass
from types import MappingProxyType
from typing import Final


@dataclass(frozen=True, slots=True)
class Profile:
    """Build and runtime parameters for one instrumentation profile."""

    build_type: str
    sanitizer: str
    mode: str
    deadline_seconds: float


STANDARD_DEADLINE_SECONDS: Final = 45.0
VALGRIND_DEADLINE_SECONDS: Final = 90.0

PROFILES: Final = MappingProxyType(
    {
        "release": Profile(
            "Release",
            "none",
            "release",
            STANDARD_DEADLINE_SECONDS,
        ),
        "asan": Profile(
            "Debug",
            "address-undefined",
            "asan",
            STANDARD_DEADLINE_SECONDS,
        ),
        "tsan": Profile(
            "Debug",
            "thread",
            "tsan",
            STANDARD_DEADLINE_SECONDS,
        ),
        "valgrind": Profile(
            "RelWithDebInfo",
            "none",
            "valgrind",
            VALGRIND_DEADLINE_SECONDS,
        ),
    },
)

PROXIES: Final = (
    "pg-proxy-1",
    "pg-proxy-2",
    "pg-proxy-3",
)

BACKENDS: Final = MappingProxyType(
    {
        "primary": "primary",
        "replica-1": "replica_1",
        "replica-2": "replica_2",
    },
)

POSTGRES_SERVICES: Final = MappingProxyType(
    {
        "primary": "postgres-primary",
        "replica-1": "postgres-replica-1",
        "replica-2": "postgres-replica-2",
    },
)

INFRASTRUCTURE_SERVICES: Final = (
    *POSTGRES_SERVICES.values(),
    *PROXIES,
)
