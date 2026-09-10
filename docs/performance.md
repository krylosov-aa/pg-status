# Performance

Measurements taken over **IPv4 loopback**.

## Confirmed RPS

pg-status was pinned to one vCPU on the 4-vCPU, 4-GB Ice Lake VM.
Accepted rates require **scheduled p99 ≤5 ms**, no HTTP/socket/timeout
or body-validation errors, and throughput ≥99% of the offered rate.

| Workload                                       |        RPS | Worst p99, ms | CPU, % of one vCPU | Peak RSS, MiB |
| ---------------------------------------------- | ---------: | ------------: | -----------------: | ------------: |
| 100% `/master`                                 | **52,500** |         4.171 |               89.6 |          9.99 |
| `/master` + `/most_sync_by_bytes`, 50:50       | **55,000** |         4.507 |               93.7 |          9.99 |
| `/master` + `/replica`, fresh RYOW, 50:50      | **52,500** |         4.499 |               93.7 |         10.00 |
| RYOW: replicas have replayed the LSN           | **47,500** |         3.923 |               85.4 |         10.00 |
| RYOW: both replicas paused; primary fallback   | **45,000** |         3.845 |               82.3 |         10.24 |
| 10% `/master`, 90% `/replica`                  | **57,500** |         4.975 |               96.6 |         10.00 |
| Application mix, text selections               | **52,500** |         4.655 |               95.3 |         10.00 |
| Same mix, JSON selections                      | **47,500** |         4.715 |               94.9 |         10.25 |
| 100% `/hosts` JSON                             | **32,500** |         4.843 |               82.9 |         10.25 |
| 100% `/master`, new TCP connection per request | **25,000** |         3.855 |               87.6 |         10.25 |

All **212,942,356 responses** in the 30 selected confirmation runs
passed the status/body checks, with no reported socket or timeout errors.
The steady throughput windows contain 196,544,929 of those responses.
The first three rows have three 310-second runs each; the rest have three
70-second runs each. CPU is the mean across their steady resource samples;
RSS is the largest sampled resident set.

The application mix is 40% `/master`, 30% `/replica` with a replayed LSN
and time/byte limits, 10% `/sync_by_time`, 10% `/most_sync_by_bytes`,
5% `/sync_by_time_and_bytes`, and 5% `/hosts`. The limits are 1,000 ms
and 1,000,000 bytes. In the JSON variant, selection requests add
`Accept: application/json`; `/hosts` always returns JSON.

The two static RYOW rows also use a 50:50 master/replica request mix.
They separate successful replica selection from primary fallback. The fresh
case continuously advances the committed LSN.

### Where the latency budget failed

The next table shows achieved throughput across the three accepted repetitions
and the nearest higher tested rate that failed. The failed p99 range includes
only failing attempts at that rate, whether in search or confirmation.

| Profile            | Achieved RPS range | Higher failed rate | Failed p99, ms |
| ------------------ | -----------------: | -----------------: | -------------: |
| `master`           |  52,499.9–52,500.2 |             55,000 |         14.807 |
| `master-most-sync` |  54,999.8–55,000.0 |             57,500 |          5.139 |
| `ryow-fresh`       |  52,499.7–52,500.1 |             55,000 |          5.387 |
| `ryow-hit`         |  47,499.5–47,500.3 |             50,000 |         68.927 |
| `ryow-fallback`    |  44,999.4–44,999.8 |             47,500 |         56.287 |
| `read-heavy`       |  57,500.0–57,501.3 |             60,000 |         20.335 |
| `mixed`            |  52,499.3–52,500.7 |             55,000 |         22.287 |
| `mixed-json`       |  47,499.1–47,500.0 |             50,000 |    5.175–5.611 |
| `hosts`            |  32,499.7–32,500.2 |             35,000 |         15.175 |
| `new-connection`   |  24,999.9–25,000.4 |             26,000 | 85.119–120.063 |

A separate observer recorded system-wide TCP counters around the six
connection-churn rechecks, with approximately one-second boundary precision.
They show 1.73–1.81 million connection opens and 7–27 retransmitted segments
per run, with no listen/backlog drops. Linux TCP settings and these counters
are included in the raw results.

### Sensitivity to the latency budget

Reclassifying the same three-run records with a 10 ms p99 budget gives the
following higher accepted rates. This post-hoc comparison changes only the
latency budget; the capacity search used 5 ms throughout.

| Profile         | p99 ≤5 ms | p99 ≤10 ms |
| --------------- | --------: | ---------: |
| `ryow-fresh`    |    52,500 |     55,000 |
| `ryow-hit`      |    47,500 |     55,000 |
| `ryow-fallback` |    45,000 |     52,500 |
| `mixed-json`    |    47,500 |     50,000 |

## CPU and memory at smaller loads

The same warmed process was measured with the application mix and no CPU
quota. Each non-idle row below is one 70-second run. Idle RAM is the snapshot
at the end of the 30-second interval.

| Offered RPS      | CPU, % of one vCPU | Peak RSS, MiB | Peak PSS, MiB | p99, ms |
| ---------------- | -----------------: | ------------: | ------------: | ------: |
| Idle, 30 seconds |               0.03 |         10.25 |          4.60 |       — |
| 1,000            |               4.18 |         10.25 |          4.60 |   1.784 |
| 5,000            |              13.61 |         10.25 |          4.60 |   1.901 |
| 10,000           |              22.83 |         10.25 |          4.59 |   2.461 |
| 20,000           |              41.33 |         10.25 |          4.59 |   2.765 |

**With a 0.1-vCPU quota**, the mixed profile confirmed **3,000 RPS** in three
70-second runs, with worst scheduled p99 of **2.523 ms**. Mean pg-status CPU
was 8.92% of one vCPU, with peak RSS of 10.30 MiB. The cgroup setting was
`cpu.max = 10000 100000` (10 ms CPU time per 100 ms period), applied to
pg-status alone. Quota search attempts and throttling counters are in the
archive. The quota was restored to unlimited after the experiment.

At 4,000 RPS under the same quota, throughput still reached the target,
but p99 rose to 50.591 ms. The cgroup was throttled in 297 of 300 periods
during that 30-second search run, for 14.36 seconds in total.

## Workloads and confirmation

RPS is the sum of all HTTP requests in a profile. For a 50:50 pair, 50,000 RPS
means 25,000 requests/s to each endpoint. PostgreSQL transactions are excluded.
The synthetic workloads are based on the [API](../README.md#api): host
selection, read-heavy routing, strict freshness, lag filters, JSON responses
and connection churn.

The reported capacity is the highest tested candidate with three passing
confirmation runs. If any confirmation fails, the rate is lowered and three
new runs are required. Initial confirmations interleave the profiles;
lower-rate rechecks run consecutively for the affected profile. All attempts
are retained. Search resolution is 2,500 RPS, or 1,000 RPS for new TCP
connections. The table uses the worst p99 of the three confirmation runs.
The raw reports also contain p50, p95, p99.9, maximum latency, corrected and
uncorrected histograms, errors and response-body counters.

## Machine and process layout

| Item                         | Configuration                                                                            |
|------------------------------|------------------------------------------------------------------------------------------|
| VM                           | Intel Xeon Processor (Icelake), KVM, x86-64                                              |
| Guest CPU topology           | 4 vCPU; guest reports 2 cores × 2 SMT threads                                            |
| Provider CPU guarantee/share | 100%                                                                                     |
| Memory                       | 4 GB VM configuration; no swap                                                           |
| Disk                         | 20 GB virtual SSD                                                                        |
| OS                           | Ubuntu 24.04.4 LTS, Linux 6.8.0-139-generic                                              |
| Build                        | Clang 18.1.3, CMake Release (`-O3 -DNDEBUG`), dynamic libraries, no sanitizers           |
| Libraries                    | libevent 2.1.12-stable, cJSON 1.7.17, libpq 16.15, glibc 2.39                            |
| pg-status                    | CPU 0, all its threads; one HTTP event-loop thread                                       |
| PostgreSQL                   | CPU 1; PostgreSQL 16.15 primary + two streaming replicas                                 |
| Generator                    | wrk2, two threads on CPUs 2–3; 128 connections                                           |
| Transport                    | HTTP/1.1, `127.0.0.1:8000`, keep-alive, no TLS/proxy/pipelining                          |
| Service settings             | 1,000 ms polling, 1,000 ms query timeout, 300,000 ms connection lifetime, info/text logs |
| Main-run limits              | No CPU quota or memory cap; file-descriptor limit 8,192                                  |

PostgreSQL runs natively, without Docker, on `127.0.0.1`, `127.0.0.2` and
`127.0.0.3`, port 5432. Each instance has 64 MiB shared buffers; fsync,
synchronous_commit and full_page_writes remain enabled. A separate writer
targets 100 committed updates/s throughout the tests, keeping WAL and
replication active. This also keeps time-lag routes meaningful: on an idle
database, the last replayed transaction timestamp ages even when no WAL
remains to replay.

The guest reports CPUs 0/1 and 2/3 as SMT siblings. pg-status has a single
HTTP event loop; adding vCPUs alone does not parallelize request handling.

In all resource tables, 100% means one logical CPU; generator
CPU is reported separately and is not included in pg-status CPU. In the
accepted runs, wrk2 used 0.76–0.97 vCPU for keep-alive workloads and 1.50 vCPU
for connection churn, from its allocation of two vCPUs. A separate 10-second
sample of the 22 PostgreSQL processes, with the writer active, measured
0.091 vCPU and 58.69 MiB total PSS; that sample excludes the writer itself.

## Measurement method

For the published capacity results, [wrk2](https://github.com/giltene/wrk2)
records latency from when each request was scheduled to start, including
backlog when the server or generator cannot keep pace. Ordinary request
latency is retained too.

The pinned wrk2 revision is `44a94c17d8e6a0bac8559b53da76848e430cb7a7`.
A one-line `<sys/time.h>` include fixes Clang 18 compilation; scheduling code
is unchanged. The Lua workload validates all statuses and host bodies;
the runner checks every path's expected role
before/after a run and verifies aggregate role proportions. The separate SQL
RYOW check verifies the write/read sequence.

wrk2 resets its latency histograms after approximately 10.32 seconds of
calibration at the default connection/thread counts. Throughput uses a common
fixed window from 12 seconds after process launch to the requested end time;
all worker threads use the same timestamps. This excludes staggered connection
startup. Raw full-run throughput and all errors are also retained. CPU/RAM
summaries use the same settling boundary. Before the long runs, both
generators were checked against slow responses and incorrect HTTP-200 bodies.

A diagnostic wrapper around [Vegeta 12.12.0](https://github.com/tsenart/vegeta/tree/v12.12.0)
reached approximately 36,000 RPS while consuming almost both allocated vCPUs,
with pg-status using only about 62% of its vCPU. This diagnostic run was
limited by the generator. The main wrk2 runs record generator CPU alongside
server CPU.

The earlier `hey -c 200 -z 30s` results used fixed concurrency and a different
measurement protocol, so they are excluded from comparisons with this series.

## RYOW workloads

All RYOW profiles use nonzero committed LSNs; `min_lsn=0/0` disables the filter.
`ryow-hit` uses an LSN that both replicas have replayed.
`ryow-fallback` pauses replay on both replicas and then captures a newer
committed LSN. The service must return the primary. Replay is resumed between
runs. `ryow-fresh` continuously uses the WAL writer's latest committed LSN;
each generator thread refreshes it every 128 requests.

With 100 commits/s and a 1-second monitoring interval, approximately
**99.3% of fresh `/replica` lookups returned the primary**. This
estimate subtracts the known 50% `/master` share from aggregate host
counters; an incomplete request tail can slightly affect it. `ryow-hit`
covers successful replica selection separately.

The writer runs independently of HTTP requests. A separate correctness check
verifies the sequence: `/master` → INSERT/commit → current WAL LSN →
`/replica?min_lsn=…` → SELECT from the returned database.

The SQL check passed before and after the load campaign: **4,200 reads in
total**. Each check included 1,000 reads immediately after commits, 100 with
replay paused, and 1,000 after replicas caught up. The first two groups selected
the primary; the last group split 500/500 across replicas. Every read saw the
committed row.

## Limitations

The rates are confirmed operating points for this VM and workload. The 5 ms
p99 budget permits slower responses in the tail and is not a production SLO.
The timer has roughly 1 ms granularity. Differences of one search step can
reflect run-to-run tail latency; they do not establish that one endpoint is
intrinsically faster than another.

Some higher-rate attempts failed on latency spikes below average CPU
saturation, including `ryow-hit` and `/hosts`. The per-second resource samples
and TCP counters did not establish their cause. Those failures were retained
and the candidate rates lowered.

CPU affinity separates virtual CPUs; physical-core isolation on the
hypervisor was not verified. The connection-churn result includes substantial
generator and TCP overhead. It describes the complete localhost setup, as
does the diagnostic Vegeta result for its particular wrapper and allocation.
Under a CPU quota, the quota period affects tail latency, so capacity must be
measured with that setting rather than scaled from the unrestricted result.

Under-load Lua responses are checked in aggregate, without matching each
response to its originating path. SQL RYOW correctness is tested separately.
Memory values describe the sampled resident memory of a warmed process;
they are not recommended container memory limits.

## Reproduce and inspect

See [the benchmark README](../test/rps/README.md) for installation, topology
setup, commands, profiles and the full suite. The tools are included here;
the historical source manifest and raw-results archive are not included in
this checkout. New runs write their results to `test/rps/results/`, which is
gitignored and must be archived separately.
