# Plan: Turn this into a resume-worthy benchmark study

## Context

Today this is a learning toy — a "Hello World" HTTP server plus a load generator. On a CV that is
worth nothing: "built a toy HTTP server in C++" is one of the most common student projects in
existence and differentiates no one.

**The deliverable is not a server — it is a benchmarking study.** The value is the measurement and
analysis: comparing concurrency architectures under load and explaining, at kernel level, *why* the
curves bend. The findings already uncovered by hand — accept-queue overflow producing ~200 ms TCP
retransmit tails at the RTO floor, thread oversubscription bending the latency curve, logging
contention polluting the hot path — are exactly what is rare on a student CV.

Target: **systems roles and low-latency / quant-dev (HFT) roles.**

**Rule:** every number in the final write-up must be one you measured and can defend for five
minutes in an interview. Nothing goes in the README that wasn't measured.

---

## Phase 0 — Correctness & hygiene

You cannot benchmark a buggy server. All of these were found during review.

### `src/server.cpp`
- [ ] `SO_REUSEADDR` via `setsockopt` between `socket()` and `bind()` — kills the `TIME_WAIT`
      "Address already in use" pain on restart.
- [ ] **Remove every `printf` from `handle_client`.** They sit *inside* the client's measured window
      and serialize on the stdout lock — they are corrupting your data right now.
- [ ] Loop `read`/`write` until complete; check return values. Don't respond to a connection whose
      read returned 0 or -1.
- [ ] `char *response` → `const char *`; delete the dead `buffer`/`response` in `main` and the
      pointless `memset` after `close`.
- [ ] Drop the unused Boost includes; add `#include <thread>` explicitly.
- [ ] Keep `usleep(100000)` but put it behind a flag — you need I/O-bound (sleep), CPU-bound (busy
      loop), and no-op modes.

### `src/load_gen.cpp`
- [ ] **Add `close(client)` on the success path.** Currently leaks an fd per request — this caps
      achievable concurrency and causes RST-driven `-1` reads on the server.
- [ ] `INADDR_ANY` → `127.0.0.1` via `inet_pton`. `0.0.0.0` is not a destination address.
- [ ] Remove `exit(EXIT_FAILURE)` from the worker — `exit()` from any thread kills the whole process
      and every sibling connection mid-flight. Return a failure result instead.
- [ ] Write loop must `break` on `-1`, not `perror`-and-continue (`bytesWritten += -1`).
- [ ] Loop `read` until it returns 0; check the result.
- [ ] `memset` the `sockaddr_in` before filling it.

### `CMakeLists.txt`
- [ ] **`TCPServer` uses `std::thread` but links only `Boost::system` — link `Threads::Threads`.**
      (Latent bug.) Drop Boost entirely; nothing uses it.
- [ ] `CMAKE_CXX_STANDARD 20` + `CMAKE_CXX_STANDARD_REQUIRED ON` (needed for `std::latch`,
      `std::counting_semaphore`).

---

## Phase 1 — Load generator becomes a real measurement tool

Keep the existing skeleton (`do_one_request` + gate + vector-of-threads + join-all) — it's sound.

- [ ] **CLI args**: host, port, concurrency (N), duration *or* request count, work-mode flag.
- [ ] **No I/O in the hot path.** Each thread accumulates latencies in its **own** vector; merge
      after `join`. Print nothing until the run ends.
- [ ] **Replace the spin-wait gate** (`while(!go.load()) continue;`) with `std::latch`. At N=1000 the
      spin burns every core and perturbs the very measurement it exists to make fair.
- [ ] **Metrics**: merge, sort, report **p50/p95/p99, min, max, mean**; **throughput (req/s)** =
      successes ÷ total wall-clock (timed in `main` around the join loop); **error breakdown by
      phase** (connect / send / recv).
- [ ] **Machine-readable output** (one CSV/JSON line per run) so the sweep script can consume it.

---

## Phase 2 — The three architectures to compare

This is the substance of the study.

1. [ ] **Thread-per-connection** — exists today. Baseline.
2. [ ] **Fixed thread pool** — N workers created once; accept loop pushes the fd into a
       `std::queue<int>` guarded by `std::mutex` + `std::condition_variable`; workers pop and handle.
       This is the fix for the oversubscription you measured.
       *(Lighter variant: keep thread-per-connection, gate spawns with a `std::counting_semaphore`.)*
3. [ ] **`epoll` event loop** — non-blocking listener; on readability loop `accept()` until `EAGAIN`
       (the real "batch accept"); non-blocking per-connection state machine.

> Pass the fd **by value** everywhere. The `&client` pointer race is fixed — don't regress it once
> fds start moving through a queue.

---

## Phase 3 — Run the sweep

- [ ] Script it. Don't run by hand. Inputs: architecture × concurrency × work-mode.
- [ ] **Concurrency**: 1, 10, 50, 100, 500, 1000.
- [ ] **Work modes**: I/O-bound (`usleep`), CPU-bound (busy loop), no-op — each exposes a different
      bottleneck (queueing vs. core count vs. accept/syscall throughput).
- [ ] Record p50 / p95 / p99 / throughput per point to CSV.
- [ ] **Control the environment**: fixed backlog, Release build, no logging, repeat each point 3×,
      record core count and kernel version. Note anything you couldn't control.
- [ ] **Reproduce the backlog tail on demand**: sweep backlog (2 vs. 700) at high concurrency to make
      the ~200 ms RTO-floor spikes appear and disappear. This is your headline finding — make it
      reproducible, not anecdotal.

---

## Phase 4 — The write-up (this is what earns the CV space)

`README.md`:
- [ ] One paragraph: what was measured, on what hardware.
- [ ] **Graphs**: p50/p99 latency vs. concurrency (one line per architecture); throughput vs.
      concurrency. This is what a reviewer actually looks at.
- [ ] **Findings, each with a mechanism** — not "epoll was faster," but *why*:
  - accept-queue overflow → dropped packet → retransmit at the kernel's 200 ms `TCP_RTO_MIN` floor
    → bimodal tail latency;
  - thread-per-connection oversubscribes cores at high N → wake-up herd queues for CPU → latency
    climbs;
  - bounded pool converts latency collapse into predictable queueing (M/M/c-like).
- [ ] Reproduction instructions.

---

## Phase 5 — Stretch (only after 0–4)

HTTP keep-alive (removes connect-per-request cost and `TIME_WAIT` churn), real request parsing,
`io_uring`. **Do not start these at the expense of the write-up.**

---

## The CV entry this produces

> **Concurrent HTTP Server & Load Generator** — C++, POSIX sockets, epoll, pthreads
> - Benchmarked three concurrency models (thread-per-connection, thread pool, epoll) across 1–1000
>   concurrent connections, measuring p50/p95/p99 latency and throughput.
> - Diagnosed bimodal tail latency as accept-queue overflow triggering TCP retransmission at the
>   kernel's minimum RTO; eliminated by tuning backlog and bounding concurrency.

To make room: merge the duplicate TFP race-detector bullets, cut the bootcamp line, trim Sellmate
6→4 bullets (drop CI/CD), trim SnackFinds 4→2 bullets (**keep the entry** — it holds the timeline),
drop Framer Motion / Three.js, and **add C++** to Languages.

---

## Verification

1. **Build**: `./run.sh` — both targets link cleanly, no `-Wwrite-strings` warnings, Threads linked
   to both.
2. **Restart test**: kill and immediately restart the server — no "Address already in use"
   (confirms `SO_REUSEADDR`). `ss -tan state time-wait | grep 8080` shows what it's bypassing.
   (`lsof` won't show these — they're kernel-held and own no process.)
3. **Correctness at N=1000**: every request succeeds; no `bytes read: 0` / `-1` on the server; fd
   count stays flat (`ls /proc/<pid>/fd | wc -l`) — proves the `close()` fix.
4. **Measurement sanity**: succeeded + failed == attempted; p50 ≤ p95 ≤ p99 ≤ max; with
   `usleep(100ms)` at low N, p50 ≈ 100 ms (the known floor).
5. **The key result reproduces**: at high N, backlog=2 shows the ~300 ms (100 + 200 RTO) tail;
   backlog=700 does not. Run 3×. If it isn't reproducible, it doesn't go in the README.
6. **Logging claim**: measure at N=500 with the server's `printf`s on vs. off — quantify it. That's
   a defensible sentence about hot-path logging.
