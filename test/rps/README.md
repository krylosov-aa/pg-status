# Manual sustainable-RPS benchmark

This benchmark sends a fixed request rate to pg-status over the IPv4 loopback
interface. It is intentionally separate from CTest because its result depends
on the host CPU, scheduler, background load, and pg-status configuration.

## Workload

The default profile models backend services asking pg-status where to connect:

| Requests | Route | Share |
|---:|---|---:|
| 8 | `/master` | 40% |
| 6 | `/replica` | 30% |
| 2 | `/sync_by_time` | 10% |
| 2 | `/most_sync_by_bytes` | 10% |
| 1 | `/replica?min_lsn=0/0` | 5% |
| 1 | `/hosts` | 5% |

Requests use HTTP keep-alive and plain-text routing responses. `/hosts` keeps a
small amount of JSON serialization in the workload. Edit `profile.txt` or set
`PROFILE_FILE` to use a workload measured from a real application.

Use `127.0.0.1`, not `localhost`: the latter may resolve to IPv6 while the
default pg-status listen address, `0.0.0.0`, is IPv4-only.

## Prerequisites

- A Release build of pg-status running on `127.0.0.1:8000` with a realistic
  PostgreSQL topology.
- [Vegeta](https://github.com/tsenart/vegeta) for fixed-rate load generation.
- `curl` and `jq`.

Do not benchmark a Debug build: this project enables sanitizers for Debug.

## Run

The default run warms up for 30 seconds and then requests 5,000 RPS for five
minutes over 128 reusable connections:

```sh
make benchmark_rps
```

Equivalent direct invocation:

```sh
./test/rps/run.sh
```

Configure the rate and SLO with environment variables:

```sh
TARGET_RPS=10000 \
MAX_P99_MS=5 \
DURATION=5m \
./test/rps/run.sh
```

The benchmark passes only when all of these conditions hold:

- success ratio is at least `MIN_SUCCESS_RATIO` (default `1.0`);
- successful throughput is at least `TARGET_RPS * MIN_THROUGHPUT_RATIO`
  (default ratio `0.99`);
- p99 latency is at most `MAX_P99_MS` (default `5`).

Available variables:

| Variable | Default |
|---|---|
| `PG_STATUS_URL` | `http://127.0.0.1:8000` |
| `TARGET_RPS` | `5000` |
| `DURATION` | `5m` |
| `WARMUP_DURATION` | `30s` |
| `CONNECTIONS` | `128` |
| `REQUEST_TIMEOUT` | `5s` |
| `MAX_P99_MS` | `5` |
| `MIN_SUCCESS_RATIO` | `1.0` |
| `MIN_THROUGHPUT_RATIO` | `0.99` |
| `PROFILE_FILE` | `test/rps/profile.txt` |
| `RESULTS_DIR` | `test/rps/results` |
| `KEEP_BINARY` | `0` |

Each run stores its profile, metadata, text report, and JSON report under
`test/rps/results/`. Set `KEEP_BINARY=1` to retain Vegeta's potentially large
binary result stream for additional reports.

## Find the limit

Run increasing rates until the first failure, then narrow the interval:

```sh
for rate in 1000 2000 5000 8000 10000 15000; do
  TARGET_RPS="${rate}" DURATION=2m ./test/rps/run.sh || break
done
```

Confirm the final candidate with at least three five-minute runs. Since the
generator and pg-status share the same machine in this localhost scenario,
also record CPU contention from the backend processes that pg-status is meant
to run alongside.
