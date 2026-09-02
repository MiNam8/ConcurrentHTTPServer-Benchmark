"""Generate instrumentation graphs from instrumentation.csv (Phase 4).

For each build mode present, produces:
  - cores_<build>.png   : cores_busy vs concurrency, one line per arch.
      This is the core-saturation picture: single-epoll sits pinned near 1.0
      (one core, its measured ceiling), while pool / hybrid / epoll-mt climb
      toward the worker/loop count as they spread work across cores.
  - threads_<build>.png : peak live thread count vs concurrency, one line per arch.
      thread-per-conn explodes with concurrency; the epoll-family stays flat
      (one loop, or a fixed pool) -- the per-connection cost made visible.

Run:  cd analysis && uv run plot_instrumentation.py
"""

import pandas as pd
import matplotlib.pyplot as plt

CSV = "../instrumentation.csv"
ARCH_ORDER = ["thread-per-conn", "pool", "epoll", "hybrid", "epoll-mt"]

df = pd.read_csv(CSV)
# vol_cs / invol_cs may be "NA" if the server's SIGTERM handler didn't fire.
for col in ("vol_cs", "invol_cs"):
    if col in df.columns:
        df[col] = pd.to_numeric(df[col], errors="coerce")


def archs_in(sub):
    return [a for a in ARCH_ORDER if a in sub.arch.unique()]


def plot_metric(build, col, ylabel, title, out, logy=False, hline=None):
    sub = df[df.build == build]
    fig, ax = plt.subplots(figsize=(7, 5))
    for arch in archs_in(sub):
        a = sub[sub.arch == arch].sort_values("concurrency")
        ax.plot(a.concurrency, a[col], marker="o", label=arch)
    if hline is not None:
        ax.axhline(hline, color="grey", ls="--", alpha=0.6,
                   label=f"{hline:g} core")
    ax.set_xscale("log")
    if logy:
        ax.set_yscale("log")
    ax.set_xlabel("concurrent connections")
    ax.set_ylabel(ylabel)
    ax.set_title(f"{title}  (build={build})")
    ax.grid(True, which="both", ls=":", alpha=0.4)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")


for build in df.build.unique():
    plot_metric(build, "cores_busy", "cores busy (cpu_s / wall_s)",
                "Core saturation vs concurrency", f"cores_{build}.png", hline=1.0)
    plot_metric(build, "peak_threads", "peak live threads",
                "Peak thread count vs concurrency", f"threads_{build}.png",
                logy=True)
    if "invol_cs" in df.columns and df[df.build == build].invol_cs.notna().any():
        plot_metric(build, "invol_cs", "involuntary context switches (whole run)",
                    "Involuntary context switches vs concurrency",
                    f"ctxsw_{build}.png", logy=True)
