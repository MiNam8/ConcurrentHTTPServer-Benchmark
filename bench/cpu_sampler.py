#!/usr/bin/env python3
"""Sample a process's CPU usage and thread count from /proc while it runs.

Usage:
    cpu_sampler.py <pid> [interval_seconds]

Polls /proc/<pid>/stat every `interval` seconds (default 0.05). On SIGTERM/SIGINT
(sent by the runner once the load finishes), or when the target exits, prints a
single summary line to stdout:

    SAMPLE,<cpu_seconds>,<wall_seconds>,<cores_busy>,<peak_threads>

  cpu_seconds  : user+system CPU time the process burned during the window.
                 /proc/<pid>/stat (the thread-group leader) accumulates the CPU
                 time of exited threads too, so this is correct even for
                 thread-per-connection, whose worker threads come and go.
  cores_busy   : cpu_seconds / wall_seconds  -> ~1.0 means one core saturated,
                 ~N means N cores busy. This is the number that exposes
                 single-epoll's one-core ceiling vs the multi-core archs.
  peak_threads : max live thread count seen (num_threads field), which spikes
                 for thread-per-conn under connection churn.
"""

import os
import sys
import time
import signal

CLK_TCK = os.sysconf("SC_CLK_TCK")   # jiffies per second (usually 100)


def read_stat(pid):
    """Return (cpu_ticks, num_threads) from /proc/<pid>/stat, or None if gone."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            data = f.read()
    except (FileNotFoundError, ProcessLookupError):
        return None
    # comm (field 2) is wrapped in parens and may contain spaces/parens itself,
    # so split on the LAST ')' and index the remaining space-separated fields.
    rparen = data.rfind(")")
    fields = data[rparen + 2:].split()
    # After ')', fields[0] is 'state' (field 3). utime=field 14, stime=field 15,
    # num_threads=field 20  ->  indices 11, 12, 17 into this list.
    utime = int(fields[11])
    stime = int(fields[12])
    num_threads = int(fields[17])
    return utime + stime, num_threads


_stop = False


def _handle(signum, frame):
    global _stop
    _stop = True


def main():
    if len(sys.argv) < 2:
        print("usage: cpu_sampler.py <pid> [interval]", file=sys.stderr)
        sys.exit(2)
    pid = int(sys.argv[1])
    interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0.05

    signal.signal(signal.SIGTERM, _handle)
    signal.signal(signal.SIGINT, _handle)

    first = read_stat(pid)
    if first is None:
        print("SAMPLE,0,0,0,0")   # process already gone
        return
    cpu0, peak_threads = first
    t0 = time.monotonic()

    last_cpu = cpu0
    while not _stop:
        time.sleep(interval)
        snap = read_stat(pid)
        if snap is None:
            break                 # target exited
        last_cpu, threads = snap
        peak_threads = max(peak_threads, threads)

    t1 = time.monotonic()
    wall = t1 - t0
    cpu_seconds = (last_cpu - cpu0) / CLK_TCK
    cores_busy = cpu_seconds / wall if wall > 0 else 0.0
    print(f"SAMPLE,{cpu_seconds:.3f},{wall:.3f},{cores_busy:.3f},{peak_threads}")


if __name__ == "__main__":
    main()
