"""Run trusted argv sequences without involving a command shell."""

import os
from collections.abc import Mapping, Sequence
from contextlib import ExitStack, chdir
from dataclasses import dataclass
from pathlib import Path
from typing import TextIO

from support.errors import E2EError
from support.streams import temporary_stream


@dataclass(frozen=True, slots=True)
class CommandResult:
    """Captured result of an external process."""

    return_code: int
    stdout: str
    stderr: str


@dataclass(frozen=True, slots=True)
class _CommandFiles:
    input_stream: TextIO
    output_stream: TextIO
    error_stream: TextIO

    def actions(self, capture: bool) -> list[tuple[int, int, int]]:
        file_actions = [
            (os.POSIX_SPAWN_DUP2, self.input_stream.fileno(), 0),
        ]
        if capture:
            file_actions.extend(
                (
                    (os.POSIX_SPAWN_DUP2, self.output_stream.fileno(), 1),
                    (os.POSIX_SPAWN_DUP2, self.error_stream.fileno(), 2),
                ),
            )
        return file_actions

    def captured(self, capture: bool) -> tuple[str, str]:
        if not capture:
            return "", ""
        return _read_stream(self.output_stream), _read_stream(
            self.error_stream
        )


class CommandRunner:
    """Execute already validated executables with explicit arguments."""

    def __init__(self, root: Path, environment: Mapping[str, str]) -> None:
        self._root = root
        self._environment = dict(environment)

    def run(
        self,
        command: Sequence[str],
        *,
        capture: bool = False,
        check: bool = True,
        stdin: str | None = None,
    ) -> CommandResult:
        """Run one command and optionally capture both output streams."""
        with chdir(self._root):
            command_result = self._run_in_root(command, capture, stdin)
        if check and command_result.return_code != 0:
            raise E2EError(_failure_message(command, command_result))
        return command_result

    def _run_in_root(
        self,
        command: Sequence[str],
        capture: bool,
        stdin: str | None,
    ) -> CommandResult:
        arguments = tuple(command)
        with ExitStack() as stack:
            command_files = _open_files(stack, stdin)
            exit_code = _spawn(
                arguments,
                self._environment,
                command_files.actions(capture),
            )
            captured = command_files.captured(capture)
        return CommandResult(exit_code, *captured)


def _open_files(stack: ExitStack, stdin: str | None) -> _CommandFiles:
    input_stream = stack.enter_context(
        temporary_stream(),
    )
    output_stream = stack.enter_context(
        temporary_stream(),
    )
    error_stream = stack.enter_context(
        temporary_stream(),
    )
    if stdin is not None:
        input_stream.write(stdin)
    input_stream.seek(0)
    return _CommandFiles(input_stream, output_stream, error_stream)


def _spawn(
    arguments: tuple[str, ...],
    environment: Mapping[str, str],
    file_actions: list[tuple[int, int, int]],
) -> int:
    process_id = os.posix_spawn(
        arguments[0],
        arguments,
        environment,
        file_actions=file_actions,
    )
    _process_id, status = os.waitpid(process_id, 0)
    return os.waitstatus_to_exitcode(status)


def _read_stream(stream: TextIO) -> str:
    stream.seek(0)
    return stream.read()


def _failure_message(
    command: Sequence[str],
    command_result: CommandResult,
) -> str:
    detail = ""
    if command_result.stdout or command_result.stderr:
        detail = (
            f"\nstdout:\n{command_result.stdout}"
            f"\nstderr:\n{command_result.stderr}"
        )
    rendered_command = " ".join(command)
    return (
        f"command failed ({command_result.return_code}): "
        f"{rendered_command}{detail}"
    )
