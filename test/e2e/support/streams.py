"""Managed temporary streams for external command execution."""

import tempfile
from collections.abc import Iterator
from contextlib import contextmanager
from typing import TextIO


@contextmanager
def temporary_stream() -> Iterator[TextIO]:
    """Yield one automatically closed anonymous text stream."""
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as stream:
        yield stream
