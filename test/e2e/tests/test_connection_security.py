"""Real PostgreSQL password and TLS connection-security scenarios."""

import pytest

from support.security import SecurityTopology

PASSWORD = r"space 'quote' \backslash password"  # noqa: S105
SECRET_MARKER = r"PG_STATUS_E2E_SECRET_MARKER_'_\_Юникод"  # noqa: S105
SECURITY_HOST = "postgres-security"
ALIVE = "alive"
MASTER = "master"


@pytest.mark.parametrize(
    ("scenario", "uses_tls", "user"),
    (
        ("disable", False, "password_monitor"),
        ("require", True, "password_monitor"),
        ("verify-full", True, "password_monitor"),
        ("mtls", True, "mtls_monitor"),
    ),
)
def test_successful_security_modes(
    security: SecurityTopology,
    scenario: str,
    uses_tls: bool,
    user: str,
) -> None:
    """Connect without TLS, with TLS, verified TLS, and required mTLS."""
    # Arrange & Act
    with security.monitor(scenario) as monitor:
        status = monitor.status(SECURITY_HOST)
        observed_tls = security.connection_uses_tls(user)
        logs = security.logs(scenario)

    # Assert
    assert status[ALIVE] is True
    assert status[MASTER] is True
    assert observed_tls is uses_tls
    assert PASSWORD not in logs
    assert "password=" not in logs


@pytest.mark.parametrize(
    ("scenario", "host", "error_marker"),
    (
        ("wrong-host", "postgres-wrong-name", "does not match host name"),
        ("unknown-ca", SECURITY_HOST, "certificate verify failed"),
    ),
)
def test_verify_full_rejects_invalid_identity(
    security: SecurityTopology,
    scenario: str,
    host: str,
    error_marker: str,
) -> None:
    """Reject a mismatched server hostname and an untrusted server CA."""
    # Arrange & Act
    with security.monitor(scenario) as monitor:
        status = monitor.status(host)
        logs = security.logs(scenario)

    # Assert
    assert status[ALIVE] is False
    assert f"operation=connect host={host}" in logs
    assert error_marker in logs
    assert PASSWORD not in logs
    assert "password=" not in logs


def test_secret_marker_is_absent_from_errors(
    security: SecurityTopology,
) -> None:
    """Never include a rejected password in connection diagnostics."""
    # Arrange & Act
    with security.monitor("bad-password") as monitor:
        status = monitor.status(SECURITY_HOST)
        logs = security.logs("bad-password")

    # Assert
    assert status[ALIVE] is False
    assert "PostgreSQL operation failed" in logs
    assert f"host={SECURITY_HOST}" in logs
    assert SECRET_MARKER not in logs
    assert "password=" not in logs


def test_probe_uses_unprivileged_login_role(
    security: SecurityTopology,
) -> None:
    """Run the production probe without superuser or replication grants."""
    # Arrange
    role_query = (
        "select rolsuper, rolreplication "
        "from pg_roles where rolname = 'password_monitor'"
    )

    # Act
    attributes = security.sql(role_query)
    with security.monitor("verify-full") as monitor:
        status = monitor.status(SECURITY_HOST)

    # Assert
    assert attributes == "f|f"
    assert status[ALIVE] is True
    assert status[MASTER] is True
