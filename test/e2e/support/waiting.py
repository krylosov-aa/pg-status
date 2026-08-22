"""Deadline-based retry helpers for eventually consistent checks."""

import time
from collections.abc import Callable

from support.errors import E2EError

RETRY_INTERVAL_SECONDS = 0.2


class _PendingResult(E2EError):
    """A retry operation completed but its result was not accepted."""


class Waiter:
    """Retry fallible operations until their result matches a predicate."""

    def __init__(self, deadline_seconds: float) -> None:
        self._deadline_seconds = deadline_seconds

    def until[ResultType](
        self,
        description: str,
        operation: Callable[[], ResultType],
        predicate: Callable[[ResultType], bool] = bool,
        *,
        timeout: float | None = None,
    ) -> ResultType:
        """Return the first accepted result or raise after the deadline."""
        if timeout is None:
            timeout = self._deadline_seconds
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                return accepted_result(operation, predicate)
            except (AssertionError, E2EError, OSError) as error:
                last_error = error
            time.sleep(RETRY_INTERVAL_SECONDS)
        raise E2EError(
            f"timed out waiting for {description}; last error={last_error}",
        )


def accepted_result[ResultType](
    operation: Callable[[], ResultType],
    predicate: Callable[[ResultType], bool],
) -> ResultType:
    """Return an accepted result or signal that it must be retried."""
    operation_result = operation()
    if predicate(operation_result):
        return operation_result
    raise _PendingResult(f"last value={operation_result!r}")
