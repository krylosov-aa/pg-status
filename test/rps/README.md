# Localhost performance measurements

The measured results and their limitations are in
[docs/performance.md](../../docs/performance.md). This directory contains the
reproduction tools. Run them separately from CTest: RPS depends on the
machine, CPU allocation, topology, transport and latency budget.

## Which runner to use

- `run.sh` / `make benchmark_rps`: a quick, fixed-rate Vegeta check against an
  existing server. It checks HTTP 200, throughput and request p99. It does not
  establish server capacity by itself: inspect generator CPU and verify the
  returned hosts separately.
- `campaign.py` + `workload.lua`: the Linux benchmark used for the published
  capacity measurements. Uses pinned wrk2, validates response bodies, records
  latency relative to the planned request schedule and samples CPU/RAM every
  second. It targets the three-host topology created below.
- `driver/`: an additional bounded-memory wrapper around Vegeta 12.12.0 used
  to diagnose generator saturation. It reports both ordinary HTTP latency and
  latency including dispatch delay. `validate-driver.py` checks that a slow
  server and an incorrect HTTP-200 body cannot pass unnoticed.

Use an optimized Release build without sanitizers. The target is
`http://127.0.0.1:8000`: plain HTTP/1.1, IPv4 loopback, no TLS or proxy.

## Prepare a dedicated Ubuntu 24.04 VM

The published run used product commit
`084563bbb314078b97f1c8fa5ce8b772a4fb1d72`, with no product-code changes.
The benchmark tools were added after that product commit. The historical
source manifest and raw-results archive are not included in this checkout,
so the commands below describe a new measurement using the current tools.
Record the product and tool revisions when comparing results.

These commands install tools and create real PostgreSQL 16 streaming replicas.
`setup-vm.sh` stops the distribution's default PostgreSQL service; use it only
on a machine dedicated to this benchmark. It refuses to overwrite an existing
benchmark data directory. All database listeners bind to loopback.

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  clang cmake ninja-build make pkg-config libpq-dev libevent-dev libcjson-dev \
  postgresql-16 postgresql-contrib python3-psycopg2 libssl-dev zlib1g-dev \
  git curl jq sysstat golang-go
cmake -S . -B cmake-builds/release -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release \
  -DPG_STATUS_SANITIZER=none -DBUILD_TESTING=ON
cmake --build cmake-builds/release -j4
ctest --test-dir cmake-builds/release --output-on-failure
bash test/rps/setup-vm.sh
bash test/rps/start-server.sh
nohup taskset -c 1 python3 test/rps/writer.py --state out/rps-state \
  >out/rps-state/writer.log 2>&1 </dev/null &
echo $! >out/rps-state/writer.pid
```

The layout is pg-status on CPU 0; PostgreSQL and a 100-commit/s WAL writer on
CPU 1; the load generator on CPUs 2–3. CPU affinity applies to all pg-status
threads, including polling and logging. There is no CPU quota or memory cap.
The polling interval is the production default, 1,000 ms. Check your own CPU
and SMT topology with `lscpu -e`; virtual CPU numbering is not portable.

Build the pinned wrk2 revision. The one-line header addition makes its source
compile with Clang 18; it does not change scheduling or latency recording.

```sh
git clone https://github.com/giltene/wrk2.git /tmp/pg-status-wrk2
cd /tmp/pg-status-wrk2
git checkout 44a94c17d8e6a0bac8559b53da76848e430cb7a7
sed -i '1i#include <sys/time.h>' src/script.c
make -j4 CC=clang
cd /path/to/pg-status
```

## Measure one scenario

```sh
ulimit -n 8192
taskset -c 1 python3 test/rps/campaign.py \
  --wrk /tmp/pg-status-wrk2/wrk \
  --scenario master --rate 40000 --duration 310
```

Each run saves its exact command, profile, environment, before/after API
responses, complete wrk2 output (including corrected and uncorrected latency
histograms), per-second resource samples and JSON result under
`test/rps/results/campaign/`. `index.jsonl` contains a compact row per run.
A run passes when:

- every received response has HTTP 200 and a body belonging to the expected
  topology; `/hosts` contains three distinct live hosts;
- no connection, read, write, status or timeout errors occurred;
- validated throughput is at least 99% of the requested rate;
- scheduled p99 is at most 5 ms (`--max-p99-ms` changes the budget).

Before and after each run, every path is checked against its profile's expected
role. Under load, body counters also show primary/replica selections. The Lua
callback does not correlate a mixed response with its originating path, so the
separate end-to-end RYOW check remains necessary.

wrk2 has an internal histogram calibration of approximately
`10 s + 5 ms × connections per thread` (10.32 s with 128 connections / two
threads). It resets latency histograms afterwards. Error counts and the raw
`full_run_rps` cover the full run. Validated RPS
uses a common wall-clock window from 12 seconds after launch to the requested
end time. All threads count responses in that same window; the denominator is
exactly `duration - 12` seconds. This excludes staggered connection startup.
Resource summaries also discard the first 12 seconds. A
310-second run therefore contains approximately five minutes of latency data.
No HTTP pipelining is used. Its timer granularity is approximately 1 ms: do not
interpret these figures as sub-millisecond latency measurements.

Find a boundary with short runs, narrow the interval, then repeat the final
candidate. A failed higher rate belongs in the report too. Do not publish a
short-run maximum as a reliable production capacity or pool percentiles across
runs: retain the worst run's p99 and show the spread.

## Workload profiles

Weights are relative HTTP request counts, not transactions. A 1:1 pair at
40,000 total RPS means 20,000 requests/s to each endpoint.

| Profile | Requests |
|---|---|
| `master` | 100% `/master` |
| `master-most-sync` | 50% `/master`, 50% `/most_sync_by_bytes` |
| `ryow-hit` | 50% `/master`, 50% `/replica?min_lsn={lsn}`, replicas caught up |
| `ryow-fallback` | Same pair, both replicas paused below a committed LSN |
| `ryow-fresh` | Same pair, latest real committed LSN changes during the run |
| `read-heavy` | 10% `/master`, 90% `/replica` |
| `mixed` | 40% master, 30% filtered replica, 10% time filter, 10% most-sync, 5% combined filter, 5% hosts JSON |
| `hosts` | 100% `/hosts` JSON serialization |

Use `--json` to request JSON on selection endpoints and `--close` to create a
new TCP connection per request. `--connections`, `--threads` and
`--generator-cpus` make transport and generator allocation explicit.

`{lsn}` must be a real nonzero committed LSN. Pass `--lsn HEX/HEX` after verifying
both replicas have replayed it. For `ryow-fresh`, pass `--fresh`; each generator
thread checks the writer's atomically replaced LSN file every 128 requests.
The writer is background traffic, not one SQL write per measured HTTP request.
These RPS figures measure routing API capacity, not SQL transaction throughput.
`min_lsn=0/0` disables the freshness filter and is not a RYOW workload.

Check real writes and reads independently:

```sh
taskset -c 1 python3 test/rps/check-ryow.py
```

It verifies 1,000 immediate reads, 100 reads with both replicas paused, then
1,000 reads after replay has caught up. Every selected database must contain
the committed row. Replica replay is resumed in a `finally` block.

## Quick Vegeta runner

Install [Vegeta](https://github.com/tsenart/vegeta), `curl` and `jq`, start a
ready server, then run:

```sh
TARGET_RPS=10000 DURATION=5m WARMUP_DURATION=30s ./test/rps/run.sh
```

The default profile is 40% master, 30% replica, 10% time-filtered replica,
10% most-sync, 5% replica with explicit time/byte filters, and 5% hosts JSON.

| Variable | Default |
|---|---|
| `PG_STATUS_URL` | `http://127.0.0.1:8000` |
| `TARGET_RPS` | `5000` |
| `DURATION` / `WARMUP_DURATION` | `5m` / `30s` |
| `CONNECTIONS` / `ATTACK_WORKERS` | `128` / `512` |
| `REQUEST_TIMEOUT` | `2s` |
| `MAX_P99_MS` | `5` |
| `MIN_SUCCESS_RATIO` / `MIN_THROUGHPUT_RATIO` | `1.0` / `0.99` |
| `PROFILE_FILE` | `test/rps/profile.txt` |
| `RESULTS_DIR` | `test/rps/results` |
| `KEEP_BINARY` | `0` |

Worker count is a generator setting; pg-status has one HTTP event-loop thread.
The script has no server-worker setting. Its standard Vegeta p99 starts when
an actual request begins, so missing the intended send schedule can be hidden
when the generator saturates. Check both achieved rate and generator headroom.
Binary results are deleted after reporting unless `KEEP_BINARY=1`; temporary
disk use can still be substantial at high RPS.

To reproduce the diagnostic Vegeta wrapper:

```sh
(cd test/rps/driver && go build -o ../pg-status-load .)
WRK=/tmp/pg-status-wrk2/wrk python3 test/rps/validate-driver.py
GOMAXPROCS=2 taskset -c 2,3 test/rps/pg-status-load \
  -profile test/rps/profiles/master.txt \
  -pid "$(cat out/rps-state/server.pid)" -rate 20000 \
  -duration 60s -warmup 10s -out /tmp/vegeta-report.json
```

## Run the complete suite

Set `WRK` to your generator binary (default: `~/bench-tools/wrk2/wrk`).
After the functional RYOW check has been saved, run the stages sequentially:

```sh
export WRK=/tmp/pg-status-wrk2/wrk
mkdir -p test/rps/results/campaign/environment
taskset -c 1 python3 test/rps/check-ryow.py \
  > test/rps/results/campaign/environment/ryow-validation.json
taskset -c 1 python3 test/rps/suite.py search
taskset -c 1 python3 test/rps/suite.py confirm
```

Search runs last 30 seconds and narrow boundaries to 2,500 RPS (1,000 RPS for
new connections). Confirmation interleaves three repetitions of every scenario.
`master`, `master-most-sync` and `ryow-fresh` run for 310 seconds per repetition;
the other scenarios run for 70 seconds. If any confirmation fails, the candidate
is lowered by one search step and three new runs are required. These lower-rate
rechecks run consecutively for that profile. Every failed
attempt remains in `index.jsonl` and `suite.json`; the confirmed rate is the
highest tested candidate with three passing confirmations, not an exact
universal maximum. The suite pauses and resumes real replica replay for the
fallback case. The WAL writer must remain running.

## Resource budget measurements

After the main suite, `extras.py` measures a 30-second warm idle baseline and
CPU/RAM at 1,000, 5,000, 10,000 and 20,000 RPS for the mixed profile. It then
moves only pg-status into a dedicated cgroup and sets `cpu.max` to
`10000 100000`: 0.1 vCPU with a 100 ms quota period. It searches for a passing
rate and requires three 70-second confirmations. The original quota is
restored in `finally`; PostgreSQL and the writer are not CPU-throttled by this
group. This step requires passwordless sudo on the dedicated test VM.

```sh
taskset -c 1 python3 test/rps/extras.py
```

`bash test/rps/confirm-all.sh` combines confirmation, these resource tests and
JSON/Markdown aggregation after a completed search. Use `WRK` to select the
pinned generator binary. No result is marked confirmed while a required run is
missing. `summarize.py` can regenerate the tables from the archived suite:

```sh
python3 test/rps/summarize.py test/rps/results/campaign \
  --output /tmp/pg-status-performance-summary.json
```

Keep the writer running during tests: time-lag routes use the last replayed
transaction timestamp, which ages on an otherwise idle database. After testing,
stop the background writer with `kill "$(cat out/rps-state/writer.pid)"`.
`extras.json` records the restored CPU quota; all replicas should have replay
resumed before reusing the topology.

## Repeat measurements

Use a separate campaign directory for each product revision and record its
build and environment. Compare the resulting RPS, latency and resource values
under the same test conditions.

### Start a separate campaign

Use the same machine allocation, CPU affinity, pinned wrk2 revision, profiles,
connection count, polling interval and 5 ms budget. Record the VM provider's
guaranteed CPU share outside the guest. Ubuntu packages installed by `apt`
can change over time; retain their versions. For a small suspected regression,
remeasure the old product revision on the same stand as well as the new one.

On an existing stand, stop any previous benchmark before changing the product.
Preserve its results, then stop the old pg-status process, rebuild the selected
revision with the Release commands above, and run `start-server.sh` again.
Check the process named in `out/rps-state/server.pid` before stopping it: PID
files can survive a VM reboot. Rebuilding alone does not replace a running
server. PostgreSQL data can be reused if all three instances are healthy and
replica replay is resumed; do not rerun `setup-vm.sh` over existing data.
After a reboot, start each existing instance with the same `taskset -c 1
pg_ctl ... -w start` commands from that script, without initdb or basebackup.

The suite has a fixed output path. Move a completed campaign aside before a
new revision; do not append another revision's confirmations to its history:

```sh
if [ -d test/rps/results/campaign ]; then
  mv test/rps/results/campaign \
    "test/rps/results/campaign-$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p test/rps/results/campaign/environment
```

Record the product identity yourself: the per-run runner records the workload
and generator hashes, but does not collect the product commit or build
inventory. Run these commands from the repository root after starting the
new binary, before measuring:

```sh
bench_env=test/rps/results/campaign/environment
git rev-parse HEAD >"$bench_env/product-commit.txt"
git status --short >"$bench_env/worktree-status.txt"
git diff HEAD -- CMakeLists.txt src >"$bench_env/product.patch"
git ls-files -z -- CMakeLists.txt src | xargs -0 sha256sum \
  >"$bench_env/product-sources.sha256"
sha256sum "/proc/$(cat out/rps-state/server.pid)/exe" \
  cmake-builds/release/src/pg-status >"$bench_env/product-binaries.sha256"
cp cmake-builds/release/CMakeCache.txt "$bench_env/"
cp out/rps-state/server-env.txt "$bench_env/"
uname -a >"$bench_env/kernel.txt"
lscpu >"$bench_env/cpu.txt"
lscpu -e >"$bench_env/cpu-topology.txt"
free -m >"$bench_env/memory.txt"
dpkg-query -W >"$bench_env/packages.txt"
clang --version >"$bench_env/compiler.txt"
ldd cmake-builds/release/src/pg-status >"$bench_env/libraries.txt"
```

The two binary hashes must match. Include any untracked product source files
listed in `worktree-status.txt` separately; `git diff` and `git ls-files` do
not capture their contents. Keep the same benchmark scripts when comparing
product changes, and retain their copy alongside each campaign.

### Check the workload, then run

Start exactly one WAL writer using the command above if it was stopped.
Before the suite, check that its PID is alive and that `latest-lsn.txt` advances
over a few seconds. A leftover LSN file alone does not establish freshness.
Rerun `check-ryow.py` and save its output for this campaign, then run `search`
followed by `confirm-all.sh`. The latter includes resource measurements and
writes `summary.json` and `summary.md`.

Before accepting the results, inspect the writer log across the measurement
interval and the fresh-RYOW reports' `lsn_refreshes`: they must show continuing
commits and LSN updates. The runner currently records LSN refreshes but does
not reject a stopped writer by itself. Preserve `writer.jsonl` with the
campaign. Run the SQL RYOW check once more after all loads and save it as
`environment/ryow-validation-final.json` before stopping the writer.

If search stops with `extend search above ...`, the entire configured rate
grid passed. Extend the relevant grid in `suite.py:search` and rerun that case
with `--cases`; this is not a measured ceiling. If execution was interrupted,
first check for a surviving wrk2 process, paused replay or a remaining CPU
quota. `finally` cleanup cannot run after SIGKILL or a VM shutdown. `confirm`
starts another three repetitions; it is not an exact checkpoint resume.

### Results for comparison

| Metric | Result file and fields |
|---|---|
| Capacity | `summary.json`: `confirmed_rps`, achieved RPS range, worst p99/p99.9 and failed higher rates |
| Resources at equal RPS | `extras.json` → `resource_curve`: CPU and RSS at 1k/5k/10k/20k RPS; individual reports also contain PSS and latency |
| Capacity under a CPU quota | `extras.json`: `quota_confirmed_rps`, its three confirmation runs and `cpu_max` |
| Run validity | Errors, body checks, writer activity, generator CPU, hardware, build, settings and individual repetitions |

Compare CPU and RAM at equal offered RPS as well as at capacity: CPU at 55k
RPS versus CPU at 45k RPS measures different amounts of work. The low-load
resource curve has one run per point; repeat a point if its difference matters.
A one-step capacity change (2,500 RPS; 1,000 for new connections) can reflect
run-to-run tail latency. Retain the repetitions and failed attempts when
interpreting such a difference.

All generated files under `test/rps/results/` are gitignored. Copy or archive
the complete campaign before deleting the VM; saving just `summary.md` loses
the evidence needed to check the result.
