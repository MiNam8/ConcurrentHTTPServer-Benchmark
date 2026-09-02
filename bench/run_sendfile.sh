#!/usr/bin/env bash
# Phase 5: read()+write() vs zero-copy sendfile() across file sizes.
# For each size x method, serve a file from the single-epoll server and drive it
# with the keep-alive load generator; record throughput (req/s) and MB/s.
# The zero-copy advantage should widen as the file grows.
set -uo pipefail

ulimit -n 65536

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${ROOT}/build/TCPServer"
LOAD_GEN="${ROOT}/build/LoadGenerator"
OUT="${ROOT}/sendfile.csv"
FILEDIR="${ROOT}/bench/files"
PORT=8080
CONC=50

# size label -> bytes
declare -A SIZES=( [1K]=1024 [100K]=102400 [1M]=1048576 [10M]=10485760 )
ORDER=(1K 100K 1M 10M)

cmake --build "${ROOT}/build" -j"$(nproc)" >/dev/null || { echo "Build failed"; exit 1; }
mkdir -p "${FILEDIR}"
echo "io,size_label,size_bytes,concurrency,requests,ok,failed,throughput_reqs,p50,p99" > "${OUT}"

wait_for_listen() { for _ in $(seq 1 50); do ss -tln 2>/dev/null | grep -q ":${PORT} " && return 0; sleep 0.1; done; return 1; }
wait_for_free()   { for _ in $(seq 1 50); do ss -tln 2>/dev/null | grep -q ":${PORT} " || return 0; sleep 0.1; done; return 1; }

for label in "${ORDER[@]}"; do
    bytes=${SIZES[$label]}
    f="${FILEDIR}/f_${label}.bin"
    [[ -f "${f}" ]] || head -c "${bytes}" /dev/urandom > "${f}"

    # ~300 MB of transfer per run, clamped to a sane request count
    requests=$(( 300000000 / bytes )); (( requests < 100 )) && requests=100; (( requests > 5000 )) && requests=5000

    for io in readwrite sendfile; do
        pkill -f "${SERVER}" 2>/dev/null; wait_for_free

        "${SERVER}" --build=none --arch=epoll --workers 1 --file "${f}" --io=${io} 1>/dev/null 2>&1 &
        srv=$!
        if ! wait_for_listen; then echo "!! server didn't start: ${io}/${label}"; kill "${srv}" 2>/dev/null; continue; fi

        line=$("${LOAD_GEN}" --concurrency "${CONC}" --requests "${requests}" --keep-alive 2>/dev/null | grep "^CSV" | sed 's/^CSV,//')
        kill "${srv}" 2>/dev/null; wait "${srv}" 2>/dev/null; wait_for_free

        if [[ -z "${line}" ]]; then echo "!! no data: ${io}/${label}"; continue; fi
        # line = concurrency,attempted,ok,failed,throughput,p50,p95,p99,max
        IFS=',' read -r _cc _att ok failed tput p50 _p95 p99 _max <<< "${line}"
        # MB/s for the console only; LC_ALL=C so the decimal is a dot, not a locale comma
        mbps=$(LC_ALL=C awk -v t="${tput}" -v b="${bytes}" 'BEGIN{printf "%.1f", t*b/1048576}')

        echo "${io},${label},${bytes},${CONC},${requests},${ok},${failed},${tput},${p50},${p99}" >> "${OUT}"
        echo "  ${io} ${label} -> ${tput} req/s, ${mbps} MB/s (ok=${ok} failed=${failed})"
    done
done

echo "wrote ${OUT}"
