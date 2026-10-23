#!/usr/bin/env bash
# Benchmark sweep: every strategy x workload mix x concurrency level, each
# against a freshly started server. Results go to results/bench.csv.
#
# Knobs (environment variables):
#   MODES     strategies, as mode[:admit]   (default: naive lockorder detect banker banker:atomic)
#   MIXES     workload mixes                (default: static balanced write)
#   CLIENTS   concurrency levels            (default: 10 50 200)
#   DURATION  measured seconds per run      (default: 8)
#   WARMUP    warm-up seconds per run       (default: 2)
#   REPS      repetitions per cell          (default: 1; use 5 for the report)
#   SEED      base RNG seed                 (default: 1)
#   OUT       output CSV                    (default: results/bench.csv)
#   UTIL=1    also record per-run resource utilization CSVs in results/raw/
set -euo pipefail
cd "$(dirname "$0")/.."

MODES=${MODES:-"naive lockorder detect banker banker:atomic"}
MIXES=${MIXES:-"static balanced write"}
CLIENTS=${CLIENTS:-"10 50 200"}
DURATION=${DURATION:-8}
WARMUP=${WARMUP:-2}
REPS=${REPS:-1}
SEED=${SEED:-1}
OUT=${OUT:-results/bench.csv}
PORT=${PORT:-20000}
SAFESERVE=${SAFESERVE:-bin/safeserve}
LOADGEN=${LOADGEN:-bin/loadgen}

[ -x "$SAFESERVE" ] && [ -x "$LOADGEN" ] || { echo "Build first: make" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")" results/raw
rm -f "$OUT"

{
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host: $(uname -srm)"
    echo "cpus: $(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
    echo "modes: $MODES"
    echo "mixes: $MIXES"
    echo "clients: $CLIENTS"
    echo "duration_s: $DURATION warmup_s: $WARMUP reps: $REPS seed: $SEED"
} > "${OUT%.csv}_meta.txt"

server_pid=""
cleanup() { [ -n "$server_pid" ] && kill -9 "$server_pid" 2>/dev/null || true; }
trap cleanup EXIT

stop_server() {
    kill -INT "$server_pid" 2>/dev/null || true
    for _ in $(seq 1 50); do
        kill -0 "$server_pid" 2>/dev/null || { server_pid=""; return; }
        sleep 0.2
    done
    kill -9 "$server_pid" 2>/dev/null || true
    server_pid=""
}

total=$(( $(wc -w <<<"$MODES") * $(wc -w <<<"$MIXES") * $(wc -w <<<"$CLIENTS") * REPS ))
n=0
for spec in $MODES; do
    mode=${spec%%:*}
    admit=incremental
    [[ $spec == *:* ]] && admit=${spec#*:}
    label=$mode
    [ "$admit" = atomic ] && label="$mode-atomic"

    for mix in $MIXES; do
        for clients in $CLIENTS; do
            for rep in $(seq 1 "$REPS"); do
                n=$((n + 1))
                # A fresh port per run: on loopback, thousands of TIME_WAIT
                # sockets left by the previous run collide with new
                # connections to the same port and stall them for ~1 s each.
                port=$((PORT + n))
                util_args=()
                [ "${UTIL:-0}" = 1 ] && util_args=(--util-csv "results/raw/util_${label}_${mix}_${clients}_${rep}.csv")

                "$SAFESERVE" --port "$port" --mode "$mode" --admit-mode "$admit" --quiet \
                    "${util_args[@]+"${util_args[@]}"}" > "results/raw/server_${label}_${mix}_${clients}_${rep}.log" 2>&1 &
                server_pid=$!
                for _ in $(seq 1 50); do
                    curl -s -o /dev/null "http://127.0.0.1:$port/health" && break
                    sleep 0.1
                done

                printf "[%2d/%d] " "$n" "$total"
                "$LOADGEN" --port "$port" --clients "$clients" --duration "$DURATION" \
                    --warmup "$WARMUP" --mix "$mix" --seed $((SEED + rep - 1)) \
                    --timeout-ms 3000 --label "$label" --csv "$OUT"
                stop_server
            done
        done
    done
done

echo "Wrote $OUT ($(($(wc -l < "$OUT") - 1)) runs). Plot with: make plot"
