# Roadmap: Designing and Optimizing a Linux HTTP Server

**Theme:** *Designing and optimizing a Linux HTTP server across I/O, CPU scheduling, and concurrency.*

This project moves from a comparison of concurrency models to **performance engineering of a
production-style HTTP server** — an engineering narrative: *baseline → benchmark → find bottleneck →
re-architect → benchmark → quantify.* Every feature serves the theme above; the goal is a coherent
systems study, not a collection of unrelated Linux APIs.

## The four groups

1. **Baseline architectures** — thread-per-connection, thread pool, single-threaded epoll *(done)*
2. **Synthesis architectures** — hybrid epoll + worker pool + eventfd; multi-threaded epoll + `SO_REUSEPORT`
3. **Systems optimizations** — HTTP keep-alive; `sendfile()` vs `read()`+`write()`
4. **Instrumentation** — throughput, p50/p95/p99, CPU utilization, context switches, event-loop
   utilization, worker utilization

Each phase below is a stoppable checkpoint. The three baseline architectures stay working throughout —
they are the "before" in the story; new architectures are added alongside, never replacing them.

---

## Progress

- **Phase 0 — Foundations** *(done)*
  - [x] Drop vestigial Boost dependency; link `Threads::Threads` to both targets
  - [x] Remove stale `build/HTTPServer` binary; confirm clean build; wire `include/`
  - [x] RAII `Fd` wrapper → `include/fd.hpp`, applied to the listening socket (builds + serves)
  - [ ] Delete remaining dead commented blocks (cosmetic — fold into the Phase 1 rewrite)
- **Phase 1 — Keep-alive + full non-blocking epoll state machine** *(done)*
  - [x] `struct Connection` + `enum ConnState` (fd, in/out, sent, state, keep_alive) — reviewed, correct
  - [x] HTTP request parser (end-of-headers `\r\n\r\n`, `Connection:` close vs keep-alive)
  - [x] Rewire epoll loop around `unordered_map<int, Connection>`; EPOLLOUT / partial-write state machine
  - [x] Keep-alive loop (multiple sequential requests per connection); drains pipelined/batched requests from `c.in` (verified: curl reuse, `nc` pipelining → 2 responses, 5000/0 under load)
  - [x] Load generator keep-alive support (`--keep-alive`: one connection/thread, `Content-Length`-framed response reader; verified +50% throughput, 5000/0)
  - [x] Apply keep-alive to the blocking archs (`handle_connection` loops until `Connection: close`/EOF; both pool + thread-per-conn; verified 5000/0, pipelining → 2). Finding: keep-alive monopolizes a pool worker per connection → effective concurrency drops to `--workers`, tail latency rises (worth surfacing in Phase 4 instrumentation).
- [x] **Phase 2 — Hybrid: epoll + worker pool + eventfd** *(synthesis centerpiece — DONE)*
  - epoll I/O thread offloads CPU work to a worker pool via `task_queue`; workers push results to `completion_queue` and wake the epoll thread through an `eventfd` registered in epoll; epoll drains completions and writes via the Phase-1 EPOLLOUT path. `--arch=hybrid`, cross-thread safety via fd-as-id (look-up-or-drop), fd parked (`mod_events(fd,0)`) while offloaded.
  - **Result (cpu, c=100, 3000 req): hybrid 4,234 req/s vs single-epoll 358 (11.8×), ≈95% of the pool's 4,469, p99 46 ms vs epoll's 290 ms, 0 failed.** Recovers multi-core throughput without the pool's keep-alive worker-monopoly problem.
- [x] **Phase 3 — Multi-threaded epoll + `SO_REUSEPORT`** *(DONE)*
  - Extracted `run_epoll_loop(Fd listener, bool hybrid)` + `make_listener(reuseport)`; `timer_to_client` now per-loop. `--arch=epoll-mt` runs N independent loops, each with its own `SO_REUSEPORT` listener, kernel load-balances connections, zero shared state / no hot-path locking.
  - **Result (cpu, c=100): workers=1 → 357 req/s (== single-epoll baseline), workers=16 → 3,487 (9.8× scaling), 0 failed.** Slightly below hybrid/pool + higher tail (p99 123 ms) because `SO_REUSEPORT` balances by *connection* (imbalance under uneven per-conn cost) vs the pool's per-*request* queue — a real comparison point for the write-up.
  - All four prior archs regression-clean (5000/0); pipelining → 2 on epoll-mt.
- [x] **Phase 4 — Instrumentation (context switches, utilization)** *(DONE)*
  - `bench/cpu_sampler.py` (/proc utime+stime -> cores_busy, peak_threads; validated in-sandbox) + `bench/run_instrumented.sh` (server+sampler+load per run -> `instrumentation.csv`). In-server `getrusage(RUSAGE_SELF)` on SIGTERM emits `RUSAGE,vol,invol,utime,stime` (aggregates exited threads). `analysis/plot_instrumentation.py` -> `cores_/threads_/ctxsw_<build>.png`.
  - **Findings (cpu):** epoll pinned at **1.0 core** across all concurrency (its 358 req/s ceiling, made visible) vs pool/hybrid ~14 cores, epoll-mt ~10–11 (SO_REUSEPORT imbalance). Context switches: **epoll ~40–50 involuntary (floor); thread-per-conn ~1 involuntary switch PER REQUEST even at c=1 (thread-churn tax, floor ~3000); pool/hybrid start cheap (~250) and rise with core contention.** NOTE: under CPU saturation thread-per-conn does NOT dwarf pool on invol_cs (both ~thousands) — its distinguishing cost is the per-request floor, not the peak.
- [x] **Phase 5 — `sendfile()` static-file serving** *(DONE)*
  - `--file <path> --io=<readwrite|sendfile>`; sendfile streams from a shared read-only fd with per-connection offset, resumed through the EPOLLOUT path (non-blocking). Correctness: readwrite/sendfile/original md5 identical at 1 MB (proves the resume reassembles a >socket-buffer response). `bench/run_sendfile.sh` + `analysis/plot_sendfile.py`.
  - **Result (single epoll, MB/s):** 1K rw 83 / sf 51 (sendfile's 2 syscalls lose on tiny files); 100K ~2000 vs 2600 (crossover); 1M 1426 vs **4464**; 10M 880 vs **3010** (~3.4×). readwrite drops past cache size (memory-bandwidth bound on the userspace copy); sendfile stays flat.
  - **Nagle/delayed-ACK war story:** the 2-syscall send (write headers + sendfile body) stalled ~40 ms/req on small files (p50 41 ms) until `TCP_NODELAY` was set on accepted sockets — 1K sendfile 1,219 → 52,606 req/s. `set_nodelay()` now applied to all accepted connections (all archs).
- [x] **Phase 6 — Serious benchmark harness (open-loop, keep-alive, `wrk` validation)** *(core DONE)*
  - `--rate R` open-loop mode fixes coordinated omission: latency measured from each request's *intended* schedule (start + i/rate), threads phase-staggered (idx/RATE) to avoid a synthetic thundering herd. Error breakdown (connect/io) + Mode line added.
  - **Result (epoll cpu, ~360 req/s ceiling, c=50):** closed-loop p99 **273 ms** vs open-loop@500 p99 **3.8 s** at the *same* actual throughput (~345) — coordinated omission = 14× hidden tail. Sanity: open-loop@100 (under capacity) p99 **7.5 ms** (no false inflation).
  - TODO (optional): `wrk` cross-check for closed-loop agreement.
- [x] **Phase 7 — Production hardening (timeouts, backpressure)** *(DONE, w/ documented shutdown gap)*
  - **Idle timeouts** (`--idle-timeout <sec>`): finite `epoll_wait` + per-loop 1 s sweep reaps connections still in `Reading` past the deadline. Fixes Slowloris — verified: partial-request connection closed after exactly 3.0 s (EOF); active connections never reaped (Working/Writing excluded).
  - **Backpressure** (`--max-conns <N>`): shed (close) new connections beyond the cap — verified 5 of 10 served at cap=5. Both opt-in (0 = disabled); default benchmark behavior byte-identical (regression 5000/0).
  - **Graceful shutdown — documented limitation:** SIGTERM handler exits cleanly (emits RUSAGE, kernel reclaims all fds -> no leaks), but full in-flight *drain* is not implemented (high-cost across the multi-loop epoll-mt for a benchmark). Honest gap.
- [x] **Phase 8 — Write-up (README restructure + performance curves)** *(DONE)*
  - README fully restructured around the theme + four groups as an engineering narrative (baseline → bottleneck → synthesis → quantify). Embeds cores/threads/ctxsw + sendfile graphs; headline table; keep-alive, sendfile+Nagle, coordinated-omission, and hardening results. `run.sh` ARCHS + `plot.py` ARCH_ORDER extended to all 5 archs (re-run `./run.sh` to refresh the baseline throughput/latency graphs with hybrid + epoll-mt lines).

---

## Phase 0 — Foundations

- **RAII `Fd` wrapper** (`include/fd.hpp`): move-only, closes in the destructor. Rule of Five —
  deleted copy, defined move (steal + invalidate source), move-assign closes existing first. `.get()`
  for syscalls (no implicit `int` conversion). Applied to the listening socket first; broader adoption
  happens in Phase 1.
- Drop the unused Boost dependency; delete dead commented blocks.
- Start using `include/` for modules (`Fd`, later `HttpParser`, `Connection`, `EventLoop`,
  `WorkerPool`, `Metrics`) — extract incrementally, no big-bang refactor.

## Phase 1 — HTTP keep-alive + full non-blocking epoll state machine

Turn the epoll server from one-request-then-close into a real event-driven HTTP/1.1 server.

- **Per-connection state** (`struct Connection`): fd, read/write buffers, `bytes_written`, parse
  state, `keep_alive`, `last_activity`.
- **HTTP/1.1 parsing**: request line + headers, end-of-headers detection, `Connection: close` vs
  keep-alive, multiple sequential requests per connection, `Content-Length` framing.
- **`EPOLLOUT` + partial writes**: on `write` → `EAGAIN`, buffer remaining bytes, `EPOLL_CTL_MOD` to
  `EPOLLOUT`, resume on writable; on completion, keep-alive → back to `EPOLLIN`, else close.
- Correct lifecycle (`EPOLL_CTL_MOD`/`DEL`, reset state between requests).
- Load generator gains keep-alive (reuse one connection for many requests, read full responses).
- Blocking archs also loop requests on one connection so all archs stay comparable.

## Phase 2 — Hybrid: epoll + worker pool + eventfd *(centerpiece)*

Separate network I/O from CPU-bound work to fix single-threaded epoll's one-core ceiling.

- One epoll thread owns all I/O; CPU work is offloaded to a worker pool via a task queue; workers
  push results to a completion queue and signal the epoll thread through an **`eventfd`** registered
  in epoll; the epoll thread drains completions and writes responses via the Phase-1 `EPOLLOUT` path.
- New `--arch=hybrid`. Correctness: connection ids (not raw pointers) across threads; epoll thread
  never blocks on queues; drain the whole completion queue per `eventfd` wake.
- **KNOWN BUG — fd reuse (open):** the task/completion queues carry the raw fd, not a connection id.
  If a client disconnects mid-compute, its fd is closed and the kernel hands the same number to the
  next `accept()`. The completion handler (`conns.find(comp.fd)` in `run_epoll_loop`) then finds the
  *new* connection and writes the old response to the wrong client. Fix: add a per-connection
  generation id (counter bumped on every accept), carry `{fd, id}` through both queues, and drop the
  completion if `conns[fd].id != comp.id`.

## Phase 3 — Multi-threaded epoll + `SO_REUSEPORT`

N independent epoll loops, each with its own `SO_REUSEPORT` listener; kernel load-balances
connections. Scales the I/O across cores (vs the hybrid, which offloads CPU work). New
`--arch=epoll-mt`; no shared connection state → no hot-path locking.

## Phase 4 — Instrumentation

- In-server: event-loop utilization (time in `epoll_wait` vs processing), worker utilization, queue
  depth, active connections, accept rate.
- OS-level: CPU time and voluntary/involuntary context switches via `getrusage` / `/proc/self/status`.
- Extend the CSV schema and `analysis/plot.py` so each architecture's bottleneck is *visible*
  (context-switch overhead, worker starvation, core saturation).

## Phase 5 — `sendfile()` static-file serving

Serve static files two ways — `read()`+`write()` vs zero-copy `sendfile()` — and benchmark across
sizes (1KB → 10MB). Non-blocking `sendfile` needs the `EPOLLOUT` resume logic (track file offset).

## Phase 6 — Serious benchmark harness

- Open-loop load option (fix coordinated omission: measure latency against *intended* send time).
- Keep-alive load + rich report (connections, requests, duration, throughput, distribution, error
  breakdown: connection / timeout / HTTP).
- Validate against `wrk` at matched settings.

## Phase 7 — Production hardening

- Graceful shutdown (`SIGINT`/`SIGTERM` via `signalfd`/self-pipe): stop accepting, drain, join, close.
- Connection idle timeouts (finite `epoll_wait` + `last_activity`) — fixes Slowloris.
- Bounded queues, connection limits, backpressure (shed load instead of unbounded growth).
- Full RAII sweep; every syscall checked.

## Phase 8 — Write-up

Restructure `README.md` around the theme and four groups as an engineering narrative; regenerate
graphs (including instrumentation, `sendfile` curves, hybrid/reuseport); update performance results.

---

## Verification (per phase — test end-to-end before advancing)

1. **Build clean** (`cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`), zero warnings.
2. **Keep-alive**: one connection serves many requests (`curl -v --http1.1`); large responses that
   exceed the socket buffer arrive complete (exercises `EPOLLOUT`); 0 failed under load.
3. **Hybrid**: `--arch=hybrid --build=cpu` at high concurrency beats single-epoll and approaches the
   pool (recovers multiple cores), 0 failed.
4. **Reuseport**: `--arch=epoll-mt` scales throughput with `--workers` up to core count.
5. **Instrumentation**: CSV/graphs show expected signatures (involuntary context switches for
   thread-per-conn; worker utilization ≈100% + growing queue for the pool under I/O; ~1 core for
   single epoll under CPU).
6. **sendfile**: zero-copy advantage widens with file size; responses byte-identical to read/write.
7. **Harness**: open-loop p99 exceeds closed-loop p99 at saturation (coordinated omission exposed);
   agrees with `wrk`.
8. **Hardening**: `SIGTERM` drains and exits cleanly (no dropped requests, no leaked fds via
   `ls /proc/<pid>/fd`); idle connections reaped after timeout; overload sheds load.
9. **Regression**: the three baseline architectures still run correctly at every phase.
