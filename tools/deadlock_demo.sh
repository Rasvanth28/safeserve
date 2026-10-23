#!/usr/bin/env bash
# Live deadlock demo against the real HTTP server.
#
# For each strategy: start SafeServe, fire 2 x POST /upload (CLOCK -> DB) and
# 4 x POST /refresh (DB -> CLOCK) at the same moment, and see what happens.
# With 4 DB connections and 2 cache locks, if the uploads grab both CLOCKs and
# the refreshes grab all four DBs, every request waits on another: deadlock.
set -uo pipefail
cd "$(dirname "$0")/.."

SAFESERVE=${SAFESERVE:-bin/safeserve}
PORT=${PORT:-18990}
BASE="http://127.0.0.1:$PORT"
TIMEOUT=4
[ -x "$SAFESERVE" ] || { echo "Build first: make" >&2; exit 1; }

run_mode() {
    local mode=$1
    echo "============================================================"
    echo " mode = $mode"
    echo "============================================================"
    "$SAFESERVE" --port "$PORT" --mode "$mode" --admit-mode incremental --quiet \
        > "/tmp/safeserve_demo_$mode.log" 2>&1 &
    local pid=$!
    for _ in $(seq 1 50); do curl -s -o /dev/null "$BASE/health" && break; sleep 0.1; done

    local out=/tmp/safeserve_demo_codes
    rm -f "$out"
    local pids=()
    for i in 1 2; do
        curl -s -m $TIMEOUT -o /dev/null -w "upload  %{http_code} %{time_total}s\n" \
            -X POST -d payload "$BASE/upload" >> "$out" &
        pids+=($!)
    done
    for i in 1 2 3 4; do
        curl -s -m $TIMEOUT -o /dev/null -w "refresh %{http_code} %{time_total}s\n" \
            -X POST "$BASE/refresh" >> "$out" &
        pids+=($!)
    done

    sleep 1
    echo "Resource table 1 s after the burst (from /metrics):"
    curl -s "$BASE/metrics" | python3 -c '
import json, sys
d = json.load(sys.stdin)
r = d["resources"]
print("  waiting requests: %d of %d active" % (r["waiting_slots"], r["active_slots"]))
for row in r["table"]:
    print("  %-6s %3d / %3d in use" % (row["name"], row["allocated"], row["total"]))
print("  deadlocks found: %d   victims aborted: %d   unsafe grants refused: %d" % (
    d["detect"]["deadlocks_found"], d["detect"]["victims_aborted"], d["banker"]["unsafe_denials"]))
' 2>/dev/null || echo "  (install python3 to pretty-print /metrics)"

    wait "${pids[@]}"
    echo "Responses (000 = no answer within ${TIMEOUT}s):"
    sort "$out" | sed 's/^/  /'
    local ok frozen
    ok=$(grep -c ' 200 ' "$out")
    frozen=$(grep -c ' 000 ' "$out")
    echo "/health still answers: $(curl -s -m 2 -o /dev/null -w '%{http_code}' "$BASE/health")"
    if [ "$frozen" -gt 0 ]; then
        echo ">>> RESULT: DEADLOCK - $frozen of 6 requests never answered."
    else
        echo ">>> RESULT: no deadlock - $ok of 6 succeeded, $((6 - ok)) got an explicit 503."
    fi
    kill -INT "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    echo
}

for mode in ${MODES:-naive banker lockorder detect}; do
    run_mode "$mode"
done
