"""Helpers for building isolated end-to-end environments."""

from typing import cast
from uuid import uuid4

import pytest

from support.environment import E2EEnvironment, default_project


def build_environment(
    request: pytest.FixtureRequest,
    *,
    isolated: bool,
) -> E2EEnvironment:
    """Build an environment for tests and optionally isolate project state."""
    profile = cast("str", request.config.getoption("--e2e-profile"))
    project = cast("str | None", request.config.getoption("--e2e-project"))
    project_name = project or default_project(profile)
    if isolated:
        suffix = uuid4().hex[:8]
        project_name = f"{project_name}-isolated-{suffix}"
    return E2EEnvironment(profile, project_name)
