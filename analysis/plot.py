"""Generate benchmark graphs from results.csv.

For each build mode present in the data, produces:
  - latency_<build>.png    : p50 and p99 latency vs concurrency
  - throughput_<build>.png : throughput vs concurrency
One line per architecture. Log x-axis (concurrency spans 1..500);
log y-axis on latency when the value range is large (CPU-bound spans ~5 orders).
"""

import pandas as pd
import matplotlib.pyplot as plt

CSV = "../results.csv"
ARCH_ORDER = ["thread-per-conn", "pool", "epoll"]

df = pd.read_csv(CSV)


def archs_in(sub):
    # keep a stable, meaningful ordering for the legend
    return [a for a in ARCH_ORDER if a in sub.arch.unique()]


def plot_latency(build):
    sub = df[df.build == build]
    fig, ax = plt.subplots(figsize=(7, 5))
    for arch in archs_in(sub):
        a = sub[sub.arch == arch].sort_values("concurrency")
        ax.plot(a.concurrency, a.p99, marker="o", label=f"{arch} p99")
        ax.plot(a.concurrency, a.p50, marker="o", linestyle="--", alpha=0.5,
                label=f"{arch} p50")
    ax.set_xscale("log")
    # CPU latencies span microseconds..seconds -> log y; none is flatter -> linear
    if sub.p99.max() / max(sub.p99.min(), 1) > 100:
        ax.set_yscale("log")
    ax.set_xlabel("concurrent connections")
    ax.set_ylabel("latency (us)")
    ax.set_title(f"Latency vs concurrency  (build={build})")
    ax.grid(True, which="both", ls=":", alpha=0.4)
    ax.legend(fontsize=8)
    fig.tight_layout()
    out = f"latency_{build}.png"
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")


def plot_throughput(build):
    sub = df[df.build == build]
    fig, ax = plt.subplots(figsize=(7, 5))
    for arch in archs_in(sub):
        a = sub[sub.arch == arch].sort_values("concurrency")
        ax.plot(a.concurrency, a.throughput, marker="o", label=arch)
    ax.set_xscale("log")
    ax.set_xlabel("concurrent connections")
    ax.set_ylabel("throughput (req/s)")
    ax.set_title(f"Throughput vs concurrency  (build={build})")
    ax.grid(True, which="both", ls=":", alpha=0.4)
    ax.legend(fontsize=9)
    fig.tight_layout()
    out = f"throughput_{build}.png"
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")


for build in df.build.unique():
    plot_latency(build)
    plot_throughput(build)
