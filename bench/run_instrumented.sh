#!/usr/bin/env bash
# Instrumented sweep: for each (arch, build, concurrency), start the server,
# sample its CPU/thread usage from /proc while the load runs, and record the
# load-gen metrics alongside cores_busy and peak_threads. This is what makes
# each architecture's bottleneck *visible* (single-epoll pinned at ~1 core,
# the multi-core archs spread across N, thread-per-conn's thread explosion).
set -uo pipefail

ulimit -n 65536

# Focused on CPU-bound work by default -- that's where the core story lives.
# Override any of these from the environment, e.g. BUILDS="none cpu".
ARCHS=(${ARCHS:-thread-per-conn pool epoll hybrid epoll-mt})
BUILDS=(${BUILDS:-cpu})
CONCURRENCIES=(${CONCURRENCIES:-1 10 50 100 200})
WORKERS=${WORKERS:-16}
REQUESTS=${REQUESTS:-3000}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${ROOT}/build/TCPServer"
LOAD_GEN="${ROOT}/build/LoadGenerator"
SAMPLER="${ROOT}/bench/cpu_sampler.py"
OUT="${ROOT}/instrumentation.csv"
PORT=8080

cmake --build "${ROOT}/build" -j"$(nproc)" >/dev/null || { echo "Build failed"; exit 1; }
echo "arch,build,workers,concurrency,ok,failed,throughput,p50,p99,cpu_seconds,cores_busy,peak_threads,vol_cs,invol_cs" > "${OUT}"

wait_for_listen() {
    for _ in $(seq 1 50); do
        ss -tln 2>/dev/null | grep -q ":${PORT} " && return 0
        sleep 0.1
    done
    return 1
}
wait_for_free() {
    for _ in $(seq 1 50); do
        ss -tln 2>/dev/null | grep -q ":${PORT} " || return 0
        sleep 0.1
    done
    return 1
}

for arch in "${ARCHS[@]}"; do
  for build in "${BUILDS[@]}"; do
    for conc in "${CONCURRENCIES[@]}"; do

        pkill -f "${SERVER}" 2>/dev/null
        wait_for_free

        srv_err=$(mktemp)   # server's SIGTERM handler prints its RUSAGE line here
        "${SERVER}" --build="${build}" --arch="${arch}" --workers "${WORKERS}" 1>/dev/null 2>"${srv_err}" &
        srv=$!
        if ! wait_for_listen; then
            echo "!! server didn't start: ${arch}/${build}/${conc}"
            kill "${srv}" 2>/dev/null
            continue
        fi

        # Start the /proc sampler against the server, capturing its summary line.
        sample_out=$(mktemp)
        python3 "${SAMPLER}" "${srv}" 0.05 > "${sample_out}" &
        sampler=$!

        # Drive the load; grab the CSV metrics line (ok,failed,throughput,p50,p95,p99,max after concurrency).
        load_line=$("${LOAD_GEN}" --concurrency "${conc}" --requests "${REQUESTS}" 2>/dev/null | grep "^CSV" | sed 's/^CSV,//')

        # Stop the sampler (prints its SAMPLE line), then the server.
        kill -TERM "${sampler}" 2>/dev/null
        wait "${sampler}" 2>/dev/null
        sample_line=$(grep "^SAMPLE," "${sample_out}" | sed 's/^SAMPLE,//')
        rm -f "${sample_out}"

        kill -TERM "${srv}" 2>/dev/null            # SIGTERM -> server prints RUSAGE, exits
        wait "${srv}" 2>/dev/null
        rusage_line=$(grep "^RUSAGE," "${srv_err}" | tail -1 | sed 's/^RUSAGE,//')
        rm -f "${srv_err}"
        wait_for_free

        if [[ -z "${load_line}" || -z "${sample_line}" ]]; then
            echo "!! no data: ${arch}/${build}/${conc} (load='${load_line}' sample='${sample_line}')"
            continue
        fi

        # load_line = concurrency,attempted,ok,failed,throughput,p50,p95,p99,max
        IFS=',' read -r _cc attempted ok failed throughput p50 p95 p99 max <<< "${load_line}"
        # sample_line = cpu_seconds,wall_seconds,cores_busy,peak_threads
        IFS=',' read -r cpu_seconds wall cores_busy peak_threads <<< "${sample_line}"
        # rusage_line = vol_cs,invol_cs,utime,stime  (empty if the handler didn't fire)
        IFS=',' read -r vol_cs invol_cs _ut _st <<< "${rusage_line}"
        vol_cs=${vol_cs:-NA}; invol_cs=${invol_cs:-NA}

        echo "${arch},${build},${WORKERS},${conc},${ok},${failed},${throughput},${p50},${p99},${cpu_seconds},${cores_busy},${peak_threads},${vol_cs},${invol_cs}" >> "${OUT}"
        echo "  ${arch} ${build} c=${conc} -> ${throughput} req/s, ${cores_busy} cores, ${peak_threads} threads, invol_cs=${invol_cs}"

    done
  done
done

echo "wrote ${OUT}"
