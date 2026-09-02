"""Phase 5 graph: read()+write() vs sendfile() throughput across file sizes.

Reads ../sendfile.csv and writes sendfile_mbps.png -- MB/s vs file size for
each I/O method. The zero-copy (sendfile) line should pull ahead of the
userspace-copy (readwrite) line as the file grows, since the per-byte cost of
the extra kernel->user->kernel copy scales with size.

Run:  cd analysis && uv run plot_sendfile.py
"""

import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv("../sendfile.csv")
# Compute MB/s here from raw columns (avoids any locale-formatted decimal issues).
df["MBps"] = df.throughput_reqs * df.size_bytes / 1048576

fig, ax = plt.subplots(figsize=(7, 5))
for io in ("readwrite", "sendfile"):
    a = df[df.io == io].sort_values("size_bytes")
    ax.plot(a.size_bytes, a.MBps, marker="o", label=io)

ax.set_xscale("log")
ax.set_xlabel("file size (bytes)")
ax.set_ylabel("throughput (MB/s)")
ax.set_title("Static-file serving: read+write vs sendfile (single epoll)")
ax.grid(True, which="both", ls=":", alpha=0.4)
ax.legend()
fig.tight_layout()
fig.savefig("sendfile_mbps.png", dpi=120)
print("wrote sendfile_mbps.png")
