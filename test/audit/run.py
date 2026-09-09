#!/usr/bin/env python3
"""Release audit with isolated inputs and durable results."""

import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME_POSTGRES_VERSION = "18"


def positive(name, default):
    value = int(os.environ.get(name, default))
    if value < 1:
        raise ValueError(f"{name} must be positive")
    return value


def boolean(name, default="1"):
    value = os.environ.get(name, default)
    if value not in ("0", "1"):
        raise ValueError(f"{name} must be 0 or 1")
    return value == "1"


def stop_process(process):
    """Terminate the whole command tree; never leave a timed-out CLI behind."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
    except ProcessLookupError:
        process.wait()
    finally:
        # The group leader can exit while a descendant ignores SIGTERM.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def run_command(command, cwd, environment, output, timeout):
    with output.open("w") as log:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            status = process.wait(timeout=timeout)
        except BaseException:
            stop_process(process)
            raise
    if status:
        raise RuntimeError(f"command exited with {status}")


def source_snapshot(root, destination):
    """Snapshot tracked and nonignored inputs for every build."""
    root = root.resolve()
    artifact_tree = destination.parent.resolve()
    try:
        listing = subprocess.check_output(
            [
                "git",
                "ls-files",
                "-z",
                "--cached",
                "--others",
                "--exclude-standard",
            ],
            cwd=root,
            timeout=30,
        )
        names = sorted(
            set(os.fsdecode(item) for item in listing.split(b"\0") if item)
        )
    except (subprocess.SubprocessError, FileNotFoundError):
        excluded = {
            ".git",
            "out",
            "cmake-builds",
            ".venv",
            ".uv-cache",
            "__pycache__",
        }
        names = []
        for base, directories, files in os.walk(root):
            directories[:] = [
                name
                for name in directories
                if name not in excluded and not name.startswith(".")
            ]
            names.extend(
                str((Path(base) / name).relative_to(root)) for name in files
            )
    manifest = {}
    for name in names:
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"invalid input path: {name}")
        if any(
            part
            in {
                "out",
                ".git",
                "__pycache__",
                ".venv",
                ".uv-cache",
                "cmake-builds",
            }
            for part in relative.parts
        ):
            continue
        source = root / relative
        if source.is_relative_to(artifact_tree):
            continue
        if not source.is_file():
            continue  # Respect local deletions.
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        manifest[name] = hashlib.sha256(target.read_bytes()).hexdigest()
    return manifest


class Audit:
    def __init__(self):
        self.run_id = (
            datetime.now(timezone.utc).strftime("%Y%m%dt%H%M%Sz")
            + f"-{os.getpid()}"
        )
        self.platform = os.environ.get("AUDIT_PLATFORM", "linux/amd64")
        if self.platform != "linux/amd64":
            raise ValueError("release audit currently supports linux/amd64")
        self.repeats = positive("AUDIT_REPEAT_COUNT", "100")
        self.emulated_repeats = positive("AUDIT_EMULATED_REPEAT_COUNT", "1")
        self.build_timeout = positive("AUDIT_BUILD_TIMEOUT", "3600")
        self.test_timeout = positive("AUDIT_TEST_TIMEOUT", "3600")
        self.pull = boolean("AUDIT_PULL")
        self.no_cache = boolean("AUDIT_NO_CACHE")
        self.versions = os.environ.get(
            "AUDIT_POSTGRES_VERSIONS", "16 17 18"
        ).split()
        if not self.versions or any(
            version not in {"16", "17", "18"} for version in self.versions
        ):
            raise ValueError(
                "AUDIT_POSTGRES_VERSIONS must select PostgreSQL 16, 17, 18"
            )
        artifact_root = Path(os.environ.get("AUDIT_ARTIFACT_ROOT", "out"))
        self.artifacts = Path(
            os.environ.get(
                "AUDIT_ARTIFACT_DIR", artifact_root / f"audit-{self.run_id}"
            )
        ).resolve()
        self.artifacts.mkdir(parents=True, exist_ok=False)
        self.source = self.artifacts / "source"
        self.logs = self.artifacts / "logs"
        self.logs.mkdir()
        self.environment = os.environ.copy()
        self.environment.update(
            {
                "BUILDKIT_PROGRESS": "plain",
                "PG_STATUS_AUDIT_RUN_ID": self.run_id,
                "PG_STATUS_E2E_ARTIFACT_DIR": str(self.artifacts / "e2e"),
                "UV_PROJECT_ENVIRONMENT": str(ROOT / "test/e2e/.venv"),
                "UV_CACHE_DIR": str(ROOT / "test/e2e/.uv-cache"),
                "PG_STATUS_E2E_SOAK_SECONDS": os.environ.get(
                    "PG_STATUS_E2E_SOAK_SECONDS", "30"
                ),
            }
        )
        self.label = f"com.pg-status.audit.run={self.run_id}"
        self.images = []
        self.report = {
            "run_id": self.run_id,
            "platform": self.platform,
            "stages": [],
            "images": {},
            "status": "running",
        }
        self.builder = ["docker", "buildx", "build", "--load"]
        self.compose = ["docker", "compose"]

    def save(self):
        (self.artifacts / "report.json").write_text(
            json.dumps(self.report, indent=2) + "\n"
        )

    def stage(
        self, name, command, *, timeout=None, environment=None, cwd=None
    ):
        print(f"==> {name}", flush=True)
        entry = {"name": name, "command": command, "status": "running"}
        self.report["stages"].append(entry)
        self.save()
        output = self.logs / f"{name}.log"
        started = time.monotonic()
        try:
            run_command(
                command,
                cwd or self.source,
                environment or self.environment,
                output,
                timeout or self.test_timeout,
            )
            entry["status"] = "passed"
        except BaseException as error:
            entry["status"] = "failed"
            entry["error"] = str(error)
            if output.exists():
                print(
                    "\n".join(
                        output.read_text(errors="replace").splitlines()[-60:]
                    ),
                    file=sys.stderr,
                )
            raise
        finally:
            entry["seconds"] = round(time.monotonic() - started, 3)
            entry["log"] = str(output.relative_to(self.artifacts))
            self.save()

    def output(self, arguments):
        return subprocess.check_output(
            arguments, text=True, timeout=60
        ).strip()

    def image(self, name):
        tag = f"pg-status-audit:{name}-{self.run_id}"
        self.images.append(tag)
        return tag

    def build(
        self,
        name,
        dockerfile,
        *,
        target=None,
        fresh=True,
        platform=None,
        args=(),
        context=None,
    ):
        image = self.image(name)
        command = [
            *self.builder,
            "--platform",
            platform or self.platform,
            "--label",
            self.label,
        ]
        if fresh and self.pull:
            command.append("--pull")
        if fresh and self.no_cache:
            command.append("--no-cache")
        if target:
            command.extend(["--target", target])
        for argument in args:
            command.extend(["--build-arg", argument])
        command.extend(
            [
                "-f",
                str(self.source / dockerfile),
                "-t",
                image,
                str(context or self.source),
            ]
        )
        self.stage(f"build-{name}", command, timeout=self.build_timeout)
        self.report["images"][image] = json.loads(
            self.output(["docker", "image", "inspect", image])
        )[0]["Id"]
        self.save()
        return image

    def container(self, name, image, command, *, platform=None):
        reports = self.artifacts / "reports" / name
        reports.mkdir(parents=True)
        self.stage(
            name,
            [
                "docker",
                "run",
                "--rm",
                "--platform",
                platform or self.platform,
                "--label",
                self.label,
                "--security-opt",
                "seccomp=unconfined",
                "--volume",
                f"{reports}:/reports",
                image,
                *command,
            ],
        )

    def e2e(
        self,
        name,
        profile,
        version,
        *,
        image=None,
        selection=None,
        fresh=False,
    ):
        environment = self.environment.copy()
        environment.update(
            {
                "PG_STATUS_POSTGRES_VERSION": version,
                "PG_STATUS_E2E_INFRA_PREFIX": (
                    f"pg-status-e2e-{self.run_id}-pg{version}"
                ),
                "PG_STATUS_E2E_HTTP_PORT": "0",
                "PG_STATUS_E2E_PROXY_1_PORT": "0",
                "PG_STATUS_E2E_PROXY_2_PORT": "0",
                "PG_STATUS_E2E_PROXY_3_PORT": "0",
                "AUDIT_PULL": str(int(fresh and self.pull)),
                "AUDIT_NO_CACHE": str(int(fresh and self.no_cache)),
            }
        )
        if image:
            environment.update(
                {
                    "PG_STATUS_E2E_IMAGE": image,
                    "PG_STATUS_E2E_PLATFORM": self.platform,
                }
            )
        else:
            environment.pop("PG_STATUS_E2E_IMAGE", None)
            environment.pop("PG_STATUS_E2E_PLATFORM", None)
        command = [
            "uv",
            "run",
            "--directory",
            "test/e2e",
            "--frozen",
            "pytest",
            "tests",
            "-s",
            "--e2e-profile",
            profile,
            "--e2e-project",
            f"pg-status-e2e-{self.run_id}-{name}",
            "--junitxml",
            str(self.artifacts / "e2e" / f"{name}.xml"),
        ]
        if selection:
            command.extend(["-k", selection])
        self.stage(name, command, environment=environment)

    def export(self, name, image, filename):
        directory = self.artifacts / name
        directory.mkdir()
        container = self.output(
            [
                "docker",
                "create",
                "--platform",
                self.platform,
                "--label",
                self.label,
                image,
                "true",
            ]
        )
        try:
            self.stage(
                f"export-{name}",
                ["docker", "cp", f"{container}:/{filename}", str(directory)],
                timeout=120,
            )
        finally:
            subprocess.run(
                ["docker", "rm", container],
                check=True,
                stdout=subprocess.DEVNULL,
                timeout=30,
            )
        path = directory / filename
        self.report.setdefault("artifacts", {})[
            str(path.relative_to(self.artifacts))
        ] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.save()

    def run(self):
        for name in ("docker", "uv", "python3", "make", "bash"):
            if not shutil.which(name):
                raise RuntimeError(f"required executable not found: {name}")
        architecture = self.output(
            ["docker", "info", "--format", "{{.Architecture}}"]
        )
        native = {
            "aarch64": "linux/arm64",
            "arm64": "linux/arm64",
            "amd64": "linux/amd64",
            "x86_64": "linux/amd64",
        }.get(architecture)
        if not native:
            raise RuntimeError(
                f"unsupported Docker architecture: {architecture}"
            )
        self.report["native_platform"] = native
        if subprocess.run(
            ["docker", "compose", "version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=30,
        ).returncode:
            self.compose = ["docker-compose"]
        if subprocess.run(
            ["docker", "buildx", "version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=30,
        ).returncode:
            if shutil.which("docker-buildx"):
                self.builder = ["docker-buildx", "build", "--load"]
            elif native == self.platform:
                self.builder = ["docker", "build"]
            else:
                raise RuntimeError(
                    "Docker buildx is required for cross-platform builds"
                )
        manifest = source_snapshot(ROOT, self.source)
        (self.artifacts / "source-manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n"
        )
        self.report["source_sha256"] = hashlib.sha256(
            json.dumps(manifest, sort_keys=True).encode()
        ).hexdigest()
        version_match = re.search(
            r"project\(pg-status VERSION (\d+\.\d+\.\d+)",
            (self.source / "CMakeLists.txt").read_text(),
        )
        if not version_match:
            raise RuntimeError("cannot read project version")
        version = version_match.group(1)
        self.environment["PG_STATUS_EXPECTED_VERSION"] = version
        self.report["configuration"] = {
            "repeat_count": self.repeats,
            "emulated_repeat_count": self.emulated_repeats,
            "postgres_versions": self.versions,
            "runtime_postgres_version": RUNTIME_POSTGRES_VERSION,
            "pull": self.pull,
            "no_cache": self.no_cache,
            "build_timeout": self.build_timeout,
            "test_timeout": self.test_timeout,
        }
        self.report["version"] = version
        self.stage(
            "compose-main",
            [
                *self.compose,
                "--env-file",
                ".env_example",
                "-f",
                "docker-compose.yml",
                "config",
                "--quiet",
            ],
            timeout=60,
        )
        self.stage(
            "compose-tests",
            [
                *self.compose,
                "--project-name",
                f"pg-status-e2e-{self.run_id}-preflight",
                "-f",
                "test/docker/docker-compose.yml",
                "--profile",
                "security",
                "--profile",
                "pg-status",
                "config",
                "--quiet",
            ],
            timeout=60,
            environment={
                **self.environment,
                "PG_STATUS_POSTGRES_VERSION": RUNTIME_POSTGRES_VERSION,
                "PG_STATUS_E2E_INFRA_PREFIX": (
                    f"pg-status-e2e-{self.run_id}-pg{RUNTIME_POSTGRES_VERSION}"
                ),
            },
        )
        for script in sorted((self.source / "test").rglob("*.sh")):
            self.stage(
                "shell-"
                + str(script.relative_to(self.source)).replace("/", "-"),
                ["bash", "-n", str(script)],
                timeout=30,
            )
        self.stage("python", ["make", "python_check"])
        target = self.build("gate", "test/pre-release/Dockerfile")
        repeats = (
            self.repeats if native == self.platform else self.emulated_repeats
        )
        self.container(
            "target-gates",
            target,
            [
                "bash",
                "/usr/local/bin/run-gates",
                f"REPEAT_COUNT={repeats}",
                f"PRE_RELEASE_TSAN={int(native == self.platform)}",
                f"PRE_RELEASE_VALGRIND={int(native == self.platform)}",
                "pre-release",
            ],
        )
        if native != self.platform:
            native_image = self.build(
                "native-gate", "test/pre-release/Dockerfile", platform=native
            )
            self.container(
                "native-gates",
                native_image,
                [
                    "bash",
                    "/usr/local/bin/run-gates",
                    f"REPEAT_COUNT={self.repeats}",
                    "test_repeat_asan",
                    "test_repeat_tsan",
                    "test_repeat",
                    "test_valgrind",
                ],
                platform=native,
            )
        else:
            native_image = target
        self.container("cmake-gcc", target, ["bash", "test/audit/cmake.sh"])
        self.container(
            "coverage-fuzz",
            native_image,
            ["bash", "test/audit/coverage-fuzz.sh"],
            platform=native,
        )
        for postgres in self.versions:
            self.e2e(
                f"postgres-{postgres}-release", "release", postgres, fresh=True
            )
        for profile in ("asan", "tsan", "valgrind"):
            self.e2e(
                f"postgres-{RUNTIME_POSTGRES_VERSION}-{profile}",
                profile,
                RUNTIME_POSTGRES_VERSION,
                fresh=(
                    profile == "asan"
                    and RUNTIME_POSTGRES_VERSION not in self.versions
                ),
            )
        runtime_images = []
        for distro in ("alpine", "ubuntu"):
            for linkage in ("shared", "static"):
                name = f"{distro}-{linkage}"
                dockerfile = f"docker/{distro}/Dockerfile_{linkage}"
                if distro == "alpine":
                    runtime_images.append((name, self.build(name, dockerfile)))
                    continue
                builder = self.build(
                    f"{name}-builder", dockerfile, target="builder"
                )
                runtime_images.append(
                    (name, self.build(name, dockerfile, fresh=False))
                )
                packager = self.build(
                    f"{name}-packager",
                    dockerfile,
                    target="packager",
                    fresh=False,
                )
                self.export(
                    linkage,
                    packager,
                    f"pg-status_{version}_linux_amd64_{linkage}.tar.gz",
                )
                if linkage == "shared":
                    deb = self.build(
                        "deb-packager",
                        "docker/ubuntu/Dockerfile_deb",
                        target="packager",
                        fresh=False,
                        args=[f"PG_STATUS_BUILDER={builder}"],
                    )
                    self.export("deb", deb, f"pg-status_{version}_amd64.deb")
        self.stage(
            "inspect-artifacts",
            [
                "docker",
                "run",
                "--rm",
                "--platform",
                self.platform,
                "--label",
                self.label,
                "--volume",
                f"{self.artifacts}:/artifacts:ro",
                target,
                "bash",
                "test/audit/inspect-artifacts.sh",
                version,
            ],
        )
        (self.artifacts / ".dockerignore").write_text(
            "*\n!shared/\n!shared/**\n!static/\n!static/**\n!deb/\n!deb/**\n"
        )
        for kind in ("shared", "static", "deb"):
            image = self.build(
                f"artifact-{kind}",
                "test/audit/Dockerfile_artifact",
                target=kind,
                fresh=False,
                args=[f"VERSION={version}"],
                context=self.artifacts,
            )
            runtime_images.append((f"artifact-{kind}", image))
        selection = " or ".join(
            (
                "test_monitor_reports_default_topology",
                "test_master_is_text_and_json",
                "test_replica_round_robin",
                "test_version_contract",
                "test_minimum_lsn_filters_lagging_replica",
                "test_query_timeout_and_recovery",
                "test_successful_security_modes",
                "test_verify_full_rejects_invalid_identity",
            )
        )
        for name, image in runtime_images:
            self.e2e(
                f"runtime-{name}",
                "release",
                RUNTIME_POSTGRES_VERSION,
                image=image,
                selection=selection,
            )
        self.report["status"] = "passed"

    def summarize_tests(self):
        results = []
        for report in sorted(self.artifacts.rglob("*.xml")):
            if self.source in report.parents:
                continue
            try:
                root = ET.parse(report).getroot()
            except ET.ParseError:
                continue
            suites = (
                [root]
                if root.tag == "testsuite"
                else list(root.iter("testsuite"))
            )
            for suite in suites:
                results.append(
                    {
                        "report": str(report.relative_to(self.artifacts)),
                        **suite.attrib,
                    }
                )
        self.report["test_suites"] = results
        self.report["conditional_checks"] = []
        for log in (self.artifacts / "reports").rglob("LastTest.log"):
            for line in log.read_text(errors="replace").splitlines():
                if "skipping IPv6" in line:
                    self.report["conditional_checks"].append(
                        {
                            "log": str(log.relative_to(self.artifacts)),
                            "message": line,
                        }
                    )

    def cleanup(self):
        """Only resources bearing this run's label can be removed."""
        failures = []
        for resource, listing, removal in (
            (
                "containers",
                ["docker", "ps", "-aq"],
                ["docker", "rm", "-f", "--volumes"],
            ),
            (
                "networks",
                ["docker", "network", "ls", "-q"],
                ["docker", "network", "rm"],
            ),
            (
                "volumes",
                ["docker", "volume", "ls", "-q"],
                ["docker", "volume", "rm"],
            ),
        ):
            try:
                ids = self.output(
                    [*listing, "--filter", f"label={self.label}"]
                ).split()
                if ids:
                    self.stage(
                        f"cleanup-{resource}",
                        [*removal, *ids],
                        cwd=ROOT,
                        timeout=120,
                    )
                if self.output([*listing, "--filter", f"label={self.label}"]):
                    raise RuntimeError(f"owned {resource} remain")
            except Exception as error:
                failures.append(f"{resource}: {error}")
        try:
            # Remove only our tags; preserve shared layers and other runs.
            tagged = self.output(
                [
                    "docker",
                    "image",
                    "ls",
                    "--filter",
                    f"label={self.label}",
                    "--format",
                    "{{.Repository}}:{{.Tag}}",
                ]
            ).split()
            owned = set(self.images)
            owned.update(tag for tag in tagged if self.owns_e2e_tag(tag))
            for tag in sorted(owned):
                inspected = subprocess.run(
                    ["docker", "image", "inspect", tag, "--format", "{{.Id}}"],
                    capture_output=True,
                    text=True,
                    timeout=30,
                    check=False,
                )
                if inspected.returncode:
                    continue  # A failed build may never have created its tag.
                self.report.setdefault("images", {})[tag] = (
                    inspected.stdout.strip()
                )
                removed = subprocess.run(
                    ["docker", "image", "rm", tag],
                    capture_output=True,
                    text=True,
                    timeout=60,
                    check=False,
                )
                if removed.returncode:
                    failures.append(f"image {tag}: {removed.stderr.strip()}")
        except Exception as error:
            failures.append(f"images: {error}")
        if failures:
            self.report["cleanup_errors"] = failures
            self.report["status"] = "failed"
        self.save()

    def owns_e2e_tag(self, tag):
        """An image label alone does not make a user-created alias our tag."""
        return tag.startswith(f"pg-status-e2e-{self.run_id}-") or tag in {
            f"pg-status-e2e:{profile}-{self.run_id}"
            for profile in ("release", "asan", "tsan", "valgrind")
        }


def main():
    audit = Audit()

    def interrupted(signum, _frame):
        raise KeyboardInterrupt(f"signal {signum}")

    signal.signal(signal.SIGTERM, interrupted)
    try:
        audit.run()
    except BaseException as error:
        audit.report["status"] = "failed"
        audit.report["error"] = str(error)
        print(f"Audit failed: {error}", file=sys.stderr)
    finally:
        audit.cleanup()
        try:
            audit.summarize_tests()
        except Exception as error:
            audit.report["status"] = "failed"
            audit.report["report_error"] = str(error)
            print(f"Cannot summarize test reports: {error}", file=sys.stderr)
        finally:
            audit.save()
    print(
        f"Audit {audit.report['status']}. "
        f"Report: {audit.artifacts / 'report.json'}"
    )
    return 0 if audit.report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
