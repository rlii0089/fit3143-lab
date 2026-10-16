# FIT3143 Lab 3 – Version 3: Reduce MPI message count

## Files

- `wsn_v3.c`: complete V3 program (Task 1 EV nodes + Task 2 base station), with function-level comments.
- `Makefile.v3`: build helper for a project containing the **existing** `common.c` and `common.h`.

**Important:** The user's `common.c` and `common.h` were not included with the message. V3 deliberately calls the **same types, function signatures, event tags and command-line parser used by the supplied V2**. Copy `wsn_v3.c` into the folder containing your own `common.c` and `common.h`. Do not compile V2 and V3 into the same executable (both contain `main`).

## Goal and strategy

V2 already avoids global collective messages and sends a neighbour request only when congested. However, a persistently congested node **repeats those requests and replies every 500 ms**, even when availability has not changed.

V3 adds change-based subscriptions:

1. On its first congested cycle, a node subscribes **once** to each actual neighbour (`TAG_V3_SUBSCRIBE`).
2. The neighbouring node sends one initial availability reply (`TAG_V3_STATUS`).
3. On future cycles, the neighbour sends an update **only if its free-port count changes**. If several status changes occur before a pending status send completes, only the most recent state is sent.
4. Congested nodes use the cached values to choose nearby redirects without repeat request messages.
5. When all cached neighbours are full, an alert is sent if the incident is new, has changed, or requires a time-window refresh. Identical ongoing incidents do not generate duplicate reports every cycle.
6. The base station receives reports and sends advice asynchronously using the same logic as V2.
7. Shutdown continues handling pending subscriptions while nodes enter a nonblocking barrier.

### Trade-off: eventual consistency

Availability updates can be in flight when a charging node decides. V3 therefore provides **eventually consistent** neighbour information, not a synchronous exact snapshot of all neighbours. This is different from V1's lockstep per-round snapshot and V2's request/response. V3 is an appropriate study of message-count vs. timeliness, but **do not claim equivalent instantaneous availability guarantees**. To require exact freshness, use the V2 polling protocol (or add an explicit revalidation handshake before every redirect/alert, which adds messages).

### Time-window refresh

The default V3 refresh is **10000 ms** (10 seconds), set with `V3_REPORT_REFRESH_MS`. It must be **shorter than the base-station report-recency window** configured by your `common.c` (typically 30000 ms). If you set a shorter `--window-ms`, recompile V3 with an appropriately shorter refresh value, for example:

```bash
mpicc -std=c11 -O2 -Wall -Wextra -fopenmp -DV3_REPORT_REFRESH_MS=1000 common.c wsn_v3.c -o wsn_v3
```

If the report-recency window is shorter than the refresh period, the base station may incorrectly treat an ongoing incident as stale.

## Compile and run

```bash
mpicc -std=c11 -O2 -Wall -Wextra -fopenmp common.c wsn_v3.c -o wsn_v3

# Match the same scenario and process count as the user's V1/V2 experiment.
mpirun --oversubscribe -np 17 ./wsn_v3 4 4 --scenario corner --rounds 3 --interval-ms 500
```

Optional helper:

```bash
make -f Makefile.v3
```

Set `OMP_NUM_THREADS` and CPU process placements consistently across all runs where applicable.

## What to compare against V1 and V2

| Measure | What to expect |
|---|---|
| `total_messages_sent` | V3 is intended to reduce this compared with V2 when neighbour availability remains unchanged. |
| `total_bytes_sent` | Fewer repeated requests/responses and duplicate reports should reduce bytes. |
| `total_port_update_cycles` | Aim for the same number across runs (typically 48 for 16 nodes × 3 cycles). |
| `total_alerts_sent` | V3 may be **lower by design**: 1 continuing incident vs. 3 periodic alerts. |
| `total_redirects_to_neighbours` | Should be similar in the unchanged `corner` scenario, but decisions are based on cached statuses. |
| `total_recommended_nodes` | V3 may have fewer total entries because it sends fewer duplicate incident reports, not because it chose different destinations. |
| `nbr_req` | For V3 it times **subscription requests**, not V2's per-cycle requests. |
| `nbr_resp` | For V3 it times **initial statuses and changed-status updates**. |
| `nbr_rtt` | For V3 it times **initial subscription setup once per node**, not every congested cycle. |
| `report_rtt` | A same-node report-to-advice latency metric; compare under similar load/polling conditions. |
| `simulation_time_sec` | V2/V3 both schedule `rounds * interval_ms`; V1 schedules roughly `(rounds-1) * interval_ms`, so adjust before comparing runtime. |

### Expected message counts for the 4×4 fixed `corner` demo

With 3 congested nodes (0, 1 and 4), and **assuming no free-port count changes**, there are 8 directed neighbour subscriptions:

- 8 subscription request messages.
- 8 initial availability replies.
- 1 congestion incident report from Node 0 (instead of 3 periodic reports).
- 1 base-station advice message (instead of 3).
- 16 termination messages.

The **illustrative** expectation is **34 application-level messages** (8+8+1+1+16), compared with your observed **70 for V2** and **406 logical message slots for V1**. This is a protocol-based expectation, **not a measured V3 result**. Actual counts may differ due to port-status changes, state changes, refreshes or the process schedule.

## Recommended validation tests

1. **Stable corner:** `4 4 --scenario corner --rounds 3 --interval-ms 500` – verify node 0 reports congestion and is advised node 5, and compare message counts.
2. **Long stable corner:** `4 4 --scenario corner --rounds 20 --interval-ms 500` – verify no unnecessary neighbour polls; note report refresh at 10 seconds is at or beyond simulation duration.
3. **Dynamic random:** `4 4 --scenario random --rounds 20 --interval-ms 500` – verify changes trigger status updates; compare decisions to V2 while accounting for transient cache staleness.
4. **All full:** `4 4 --scenario full --rounds 5 --interval-ms 500` – verify no suitable alternative is reported.
5. **Different topologies:** `1 5` and `3 3`, especially edge/corner neighbour counts.
6. **Shutdown stress:** Small `--interval-ms` and high congestion; run repeatedly to detect unfinished requests, unexpected MPI errors or hangs.

Run more than one repetition, capture `total_messages_sent` and `total_bytes_sent`, and use medians/averages for delays. `--oversubscribe` may introduce scheduler noise.

## Testing status

The V3 translation unit passed a strict GCC **C11 syntax-only check** with an MPI/common API declaration shim (`-Wall -Wextra -Wpedantic -Werror`). The actual `common.c` / `common.h` and MPI implementation are not available in this session, so this is **not a verified MPI build or runtime test**. Compile it against your actual project and resolve any differences in the shared declarations before relying on the results.
