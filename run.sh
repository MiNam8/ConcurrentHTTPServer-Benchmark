#!/usr/bin/env bash
set -uo pipefail

ulimit -n 4096

ARCHS=("thread-per-conn" "pool" "epoll")
BUILDS=("none" "cpu" "sleep")
CONCURRENCIES=(1 10 50 100 200 500)
WORKERS=16
REQUESTS=3000

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER="${ROOT}/build/TCPServer"
LOAD_GEN="${ROOT}/build/LoadGenerator"
OUT="${ROOT}/results.csv"
PORT=8080

cmake -S "${ROOT}" -B "${ROOT}/build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "${ROOT}/build" -j"$(nproc)" || { echo "Build failed"; exit 1; }
echo "arch,build,workers,concurrency,attempted,ok,failed,throughput,p50,p95,p99,max" > "${OUT}"

wait_for_listen()
{
    for _ in $(seq 1 50); do
        ss -tln 2>/dev/null | grep -q ":${PORT} " && return 0
        sleep 0.1
    done
    return 1
}

wait_for_free()
{
    for _ in $(seq 1 50); do
        ss -tln 2>/dev/null | grep -q ":${PORT} " || return 0
        sleep 0.1
    done
    return 1
}


# pkill -f "${SERVER}"
# wait_for_free

# "${SERVER}" --build=none --arch=pool --workers "${WORKERS}" 1>/dev/null 2>&1 &
# srv=$!

# wait_for_listen || { echo "Server did not start"; kill "${srv}" 2>/dev/null; exit 1; }

# line=$("${LOAD_GEN}" --concurrency 50 --requests "${REQUESTS}" 2>/dev/null | grep "^CSV," | sed 's/^CSV,//')

# kill "${srv}" 2>/dev/null
# wait "${srv}" 2>/dev/null
# wait_for_free

# echo "pool,none,${WORKERS},${line}" >> "${OUT}"
# echo "got: ${line}"
for arch in "${ARCHS[@]}"; do
  for build in "${BUILDS[@]}"; do
    for conc in "${CONCURRENCIES[@]}"; do

        pkill -f "${SERVER}" 2>/dev/null
        wait_for_free

        "${SERVER}" --build="${build}" --arch="${arch}" --workers "${WORKERS}" 1>/dev/null 2>&1 &
        srv=$!

        if ! wait_for_listen; then
            echo "!! server didn't start: ${arch}/${build}/${conc}"
            kill "${srv}" 2>/dev/null
            continue
        fi

        line=$("${LOAD_GEN}" --concurrency "${conc}" --requests "${REQUESTS}" 2>/dev/null | grep "^CSV" | sed 's/^CSV,//')

        kill "${srv}" 2>/dev/null
        wait "${srv}" 2>/dev/null
        wait_for_free

        if [[ -n "${line}" ]]; then
            echo "${arch},${build},${WORKERS},${line}" >> "${OUT}"
            echo "  ${arch} ${build} conc=${conc} -> ${line}"
        else
            echo "!! no data: ${arch}/${build}/${conc}"
        fi

    done
  done
done
