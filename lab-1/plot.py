"""
Reads results.csv (produced by benchmark.c) and produces 5 graphs:

  1. Serial vs pthreads (2, 4, 8 threads)         -- line, x=n
  2. Serial vs OpenMP static (2, 4, 8 threads)    -- line, x=n
  3. Serial vs OpenMP dynamic (2, 4, 8 threads)   -- line, x=n
  4. pthreads vs OMP static vs OMP dynamic, n fixed at max(n) in the
     data (should be 100,000,000 if using the full benchmark run)
     -- grouped bar chart, x=thread count
  5. pthreads vs OMP static vs OMP dynamic, threads fixed at 4
     -- line, x=n

Usage:
    install dependencies:
    python3 -m venv venv
    source venv/bin/activate
    pip install pandas matplotlib

    then to run:
    python3 plot.py [path/to/results.csv]

Output: 5 PNG files in the current directory.
"""
import sys
import pandas as pd
import matplotlib.pyplot as plt

csv_path = sys.argv[1] if len(sys.argv) > 1 else "results.csv"
df = pd.read_csv(csv_path)

# ---------- helpers ----------
def n_in_millions(series):
    return series / 1_000_000

def save(fig, name):
    fig.tight_layout()
    fig.savefig(name, dpi=150)
    print(f"wrote {name}")
    plt.close(fig)

serial = df[df.config == "serial"].sort_values("n")

# ================= Graph 1: Serial vs pthreads =================
fig, ax = plt.subplots(figsize=(9, 6))
ax.plot(n_in_millions(serial.n), serial.time, marker="o", markersize=4, label="Serial")
for t in (2, 4, 8):
    sub = df[(df.config == "pthread") & (df.threads == t)].sort_values("n")
    ax.plot(n_in_millions(sub.n), sub.time, marker="o", markersize=4, label=f"POSIX ({t} threads)")
ax.set_xlabel("n (millions)")
ax.set_ylabel("Computation time (s)")
ax.set_title("Serial vs POSIX Threads")
ax.legend()
ax.grid(alpha=0.3)
save(fig, "graph1_serial_vs_posix.png")

# ================= Graph 2: Serial vs OMP static =================
fig, ax = plt.subplots(figsize=(9, 6))
ax.plot(n_in_millions(serial.n), serial.time, marker="o", markersize=4, label="Serial")
for t in (2, 4, 8):
    sub = df[(df.config == "omp_static") & (df.threads == t)].sort_values("n")
    ax.plot(n_in_millions(sub.n), sub.time, marker="o", markersize=4, label=f"OMP static ({t} threads)")
ax.set_xlabel("n (millions)")
ax.set_ylabel("Computation time (s)")
ax.set_title("Serial vs OpenMP (Static Scheduling)")
ax.legend()
ax.grid(alpha=0.3)
save(fig, "graph2_serial_vs_omp_static.png")

# ================= Graph 3: Serial vs OMP dynamic =================
fig, ax = plt.subplots(figsize=(9, 6))
ax.plot(n_in_millions(serial.n), serial.time, marker="o", markersize=4, label="Serial")
for t in (2, 4, 8):
    sub = df[(df.config == "omp_dynamic") & (df.threads == t)].sort_values("n")
    ax.plot(n_in_millions(sub.n), sub.time, marker="o", markersize=4, label=f"OMP dynamic ({t} threads)")
ax.set_xlabel("n (millions)")
ax.set_ylabel("Computation time (s)")
ax.set_title("Serial vs OpenMP (Dynamic Scheduling)")
ax.legend()
ax.grid(alpha=0.3)
save(fig, "graph3_serial_vs_omp_dynamic.png")

# ================= Graph 4: fixed n (largest n in data), grouped bar =================
n_fixed = df.n.max()
fig, ax = plt.subplots(figsize=(9, 6))
thread_counts = [2, 4, 8]
configs = ["pthread", "omp_static", "omp_dynamic"]
labels = ["POSIX", "OMP static", "OMP dynamic"]
width = 0.25
x = range(len(thread_counts))

for i, (cfg, label) in enumerate(zip(configs, labels)):
    sub = df[(df.config == cfg) & (df.n == n_fixed)].set_index("threads").reindex(thread_counts)
    offsets = [xi + (i - 1) * width for xi in x]
    ax.bar(offsets, sub.time, width=width, label=label)

ax.set_xticks(list(x))
ax.set_xticklabels([str(t) for t in thread_counts])
ax.set_xlabel("Number of threads")
ax.set_ylabel("Computation time (s)")
ax.set_title(f"POSIX vs OMP Static vs OMP Dynamic (n = {n_fixed:,})")
ax.legend()
ax.grid(alpha=0.3, axis="y")
save(fig, "graph4_fixed_n_by_threads.png")

# ================= Graph 5: fixed threads=4, varying n =================
fig, ax = plt.subplots(figsize=(9, 6))
for cfg, label in zip(configs, labels):
    sub = df[(df.config == cfg) & (df.threads == 4)].sort_values("n")
    ax.plot(n_in_millions(sub.n), sub.time, marker="o", markersize=4, label=label)
ax.set_xlabel("n (millions)")
ax.set_ylabel("Computation time (s)")
ax.set_title("POSIX vs OMP Static vs OMP Dynamic (4 threads)")
ax.legend()
ax.grid(alpha=0.3)
save(fig, "graph5_fixed_threads_by_n.png")

print("\nAll graphs generated.")