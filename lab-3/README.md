# FIT3143 Lab 3 – Tasks 1 and 2

This is a combined **C / Open MPI / OpenMP** simulation of a distributed EV charging network.
The procedural C structure, `MPI_Cart_create` / `MPI_Cart_shift` calls, `#define` constants,
and explanatory function comments follow the approach in the Week 11 lab preparation and
`cannon.c` / `fox.c` lecture examples.

## Files

- `lab3_wsn.c` – full implementation of Tasks 1 and 2.
- `Makefile` – build instructions.
- `run_demo.sh` – sample 3x3 demonstration, including congestion and recommendations.

## Requirements

- C11 compiler
- Open MPI (or an MPI-3 compatible implementation)
- OpenMP support in the compiler

Tested with a **strict syntax/warning check** using GCC and a local MPI declarations header.
**An MPI runtime was not installed in the file-creation environment**, so real multi-process
execution has **not** been verified here. Compile and test on the Monash CAAS cluster or an
MPI-enabled Linux machine before submitting.

## Build

```bash
make
# equivalent:
mpicc -std=c11 -O2 -Wall -Wextra -fopenmp lab3_wsn.c -o lab3_wsn
```

## Run

```bash
# One MPI process per charging node PLUS one base-station MPI process.
# This example uses 9 charging nodes (3x3) + 1 base station = 10 processes.
export OMP_NUM_THREADS=2
mpirun -np 10 ./lab3_wsn 3 3 --ports 6 --threshold 1 \
    --rounds 5 --interval-ms 1000 --window-sec 30 \
    --scenario demo --log base_station.log
```

If `mpirun` on a local machine complains about available CPU slots, use
`--oversubscribe` **for local functional testing only** (not for meaningful
performance measurements), or request the correct number of slots on the CAAS
cluster.

## CLI inputs

| Input | Meaning | Default |
|---|---|---|
| `ROWS COLS` | Required grid dimensions. Use `1 N` for a line. | Required |
| `--ports K` | Charging ports at each node. | 6 |
| `--threshold T` | Node is congested when its free ports are less than `T`. | 1 |
| `--rounds R` | Fixed number of simulation rounds before graceful stop. | 10 |
| `--interval-ms MS` | Delay between simulation rounds, in milliseconds. | 1000 |
| `--window-sec S` | How long previous neighbour alerts remain recent. | 30 |
| `--arrival P` | Chance (percent) a free port becomes occupied in a round. | 45 |
| `--departure P` | Chance (percent) an occupied port becomes free in a round. | 35 |
| `--scenario NAME` | `random`, `demo`, or `full`. | random |
| `--seed N` | Random seed used for reproducible port simulation. | 12345 |
| `--log PATH` | Base-station log file path. | base_station.log |

The process count **must equal** `ROWS * COLS + 1`. A bad count or invalid option
prints usage and terminates normally.

## Scenarios

- `--scenario random`: port states change probabilistically (arrivals and departures).
- `--scenario demo`: centre station and its adjacent stations are congested; farther
  stations remain available. For **3x3** and threshold=1, Node 4 reports congestion,
  and the base station can recommend all four corners (0, 2, 6 and 8). Ports at available
  nodes periodically rotate their free positions.
- `--scenario full`: all stations have zero free ports. Use this to check the
  no-available-nodes advice path.

## Overall flow

1. MPI initializes and reserves the last world rank for the base station.
2. Other ranks form an `MPI_Cart_create()` 2D non-periodic communicator.
3. Base station broadcasts `RUN` at the start of each round.
4. Each charging MPI process updates an integer port-status array using OpenMP.
5. Charging nodes exchange **request flags** and **availability responses** through
   two `MPI_Neighbor_alltoall()` calls. Only congested nodes set a request flag.
   Non-congested nodes still service incoming neighbour requests.
6. Every charging node contributes a fixed-size status/alert record to one
   `MPI_Gather()` operation. This is both a periodic **status snapshot** and, when
   `alert=1`, a detailed **congestion report**. Base station receives snapshots
   from all charging nodes every round.
7. If a node has free capacity below threshold and none of its immediate neighbours
   have sufficient capacity, its `alert` flag is set. The base station examines
   its report and previous neighbour alerts in a configurable time window.
8. The base station sends the count and list of **all equally nearest** available
   stations back to every reporting node. Distance is Manhattan grid distance.
   If all of the reporting node's immediate neighbours have sent recent alerts,
   it instead reports that no nearby options are available (the specified rule).
9. The base station writes `base_station.log` and, after the chosen number of
   rounds, broadcasts `STOP`. All processes release resources and call
   `MPI_Finalize()`.

## Log output

`base_station.log` includes:

- Simulation configuration and elapsed time
- Every charging-node rank, coordinates, timestamp, occupied/free counts
- Direct-neighbour ranks and free-port counts during reported congestion
- Incoming request counts, neighbour message counts and payload byte sizes
- Time spent exchanging neighbour availability in each round
- Incident alerts and all recommended candidate nodes
- Total alerts, message-slot counts, snapshots, advice messages and overall run time

**Message count note:** `MPI_Neighbor_alltoall` exchanges one integer slot with each
valid neighbour in *each* of two collectives, including zero request flags and
`-1` responses. Logged counts therefore describe **logical per-neighbour payloads**,
not hidden MPI collective protocol traffic. Timing counts measured with
`MPI_Wtime()` cover **both** neighbour collective calls together and are not
one-way network latency; Task 3 needs separately instrumented timings for each
message type.

## Design decisions and limitations

- Only the main thread of an MPI process calls MPI (`MPI_THREAD_FUNNELED`).
- OpenMP port updates are safe because each thread modifies a distinct array
  element, and the free-port total is obtained with a reduction.
- `MPI_Cart_create(..., reorder=0, ...)` preserves charging-node world rank order.
- `MPI_Gather()` includes the base station's dummy contribution: the base-station
  receiving array allocates `ROWS * COLS + 1` records.
- Global per-round snapshots give the base station fresh availability information,
  even from nodes that did not report congestion. This is a deliberate design
  choice beyond the minimum incident reporting requirement.
- The time-window interpretation is: if **all** adjacent nodes have reported
  congestion within the configured period, return “no nodes available nearby”;
  otherwise choose all nearest eligible candidates from the latest snapshots.
- The base station runs only one MPI process, with one thread issuing MPI calls.
- This is a **functional baseline for Tasks 1 and 2**, not the full HD optimisation
  comparison. The rubric additionally rewards measured blocking/nonblocking
  variants (multiple alternatives) and large-scale Task 3 experiments.
- The program assumes a homogeneous MPI environment when transferring packed
  C structures as `MPI_BYTE`. A heterogeneous cluster needs a matching derived
  datatype or explicit serialization.
- The simulation is discrete-round based, not a real-time EV queueing system;
  per-port status is a synthetic arrival/departure proxy.

## Suggested functional checks on an MPI machine

```bash
# Expected: one centre alert with four corner candidates (3x3).
mpirun -np 10 ./lab3_wsn 3 3 --scenario demo --rounds 3 --log demo.log

# Expected: alerts, but no available node recommendations.
mpirun -np 10 ./lab3_wsn 3 3 --scenario full --rounds 3 --log full.log

# Expected: random behaviour; adjust threshold to generate more incidents.
mpirun -np 10 ./lab3_wsn 3 3 --scenario random --threshold 3 --rounds 15 --log random.log

# Verify line topology and non-square grids.
mpirun -np 6 ./lab3_wsn 1 5 --scenario random --rounds 3 --log line.log
mpirun -np 13 ./lab3_wsn 3 4 --scenario demo --rounds 3 --log rectangle.log
```

Before submitting: confirm logs, check MPI shutdown without manual termination,
run your code on CAAS, and prepare your required AI declaration and prompt records.
