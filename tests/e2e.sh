#!/usr/bin/env bash
# End-to-end HTTP tests against a real server process.
set -uo pipefail
cd "$(dirname "$0")/.."

SAFESERVE=${SAFESERVE:-bin/safeserve}
PORT=${PORT:-18181}
BASE="http://127.0.0.1:$PORT"
fail=0

check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "[e2e] $1: PASS"; else echo "[e2e] $1: FAIL (expected '$2', got '$3')"; fail=1; fi
}
code() { curl -s -o /dev/null -w "%{http_code}" "$@"; }

start() {
    "$SAFESERVE" --port "$PORT" --quiet "$@" > /tmp/safeserve_e2e.log 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do curl -s -o /dev/null "$BASE/health" && return; sleep 0.1; done
    echo "server did not start"; cat /tmp/safeserve_e2e.log; exit 1
}
stop() {
    kill -INT "$PID" 2>/dev/null
    for _ in $(seq 1 60); do kill -0 "$PID" 2>/dev/null || return 0; sleep 0.1; done
    echo "[e2e] graceful shutdown: FAIL (still running)"; kill -9 "$PID"; fail=1
}

start --mode banker
check "GET /health"                  200 "$(code "$BASE/health")"
check "HEAD /health"                 200 "$(code -I "$BASE/health")"
check "GET / (index)"                200 "$(code "$BASE/")"
check "GET /static/index.html"       200 "$(code "$BASE/static/index.html")"
check "Content-Type of index"        "text/html; charset=utf-8" "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/")"
check "GET /static/missing"          404 "$(code "$BASE/static/nope.txt")"
check "Traversal blocked"            404 "$(code --path-as-is "$BASE/static/../config/server.conf")"
check "Encoded traversal blocked"    404 "$(code "$BASE/static/%2e%2e/config/server.conf")"
check "Unknown route"                404 "$(code "$BASE/nope")"
check "GET /query?id=1"              200 "$(code "$BASE/query?id=1")"
check "POST /upload"                 200 "$(code -X POST -d hello "$BASE/upload")"
check "POST /refresh"                200 "$(code -X POST "$BASE/refresh")"
check "GET /upload -> 405"           405 "$(code "$BASE/upload")"
check "Allow header on 405"          "POST" "$(curl -s -D - -o /dev/null "$BASE/upload" | tr -d '\r' | awk -F': ' 'tolower($1)=="allow"{print $2}')"
check "DELETE / -> 405"              405 "$(code -X DELETE "$BASE/")"
check "Malformed request -> 400"     400 "$(printf 'NONSENSE\r\n\r\n' | nc -w 2 127.0.0.1 "$PORT" | head -1 | awk '{print $2}')"
check "Chunked body -> 501"          501 "$(code -X POST -H 'Transfer-Encoding: chunked' -d x "$BASE/upload")"
# From a file: Linux caps a single command-line argument at 128 KB.
head -c 200000 /dev/zero | tr '\0' 'a' > /tmp/safeserve_e2e_big
check "200 KB upload (multi-recv)"   '"bytes_received":200000' "$(curl -s -X POST --data-binary @/tmp/safeserve_e2e_big "$BASE/upload" | grep -o '"bytes_received":[0-9]*')"
rm -f /tmp/safeserve_e2e_big
check "Split request (slow client)"  200 "$( (printf 'GET /health HTTP/1.1\r\n'; sleep 0.3; printf 'Host: x\r\n\r\n') | nc -w 2 127.0.0.1 "$PORT" | head -1 | awk '{print $2}')"

metrics=$(curl -s "$BASE/metrics")
if command -v python3 >/dev/null; then
    check "/metrics is valid JSON" ok "$(python3 -c 'import json,sys; d=json.load(sys.stdin); print("ok" if d["mode"]=="banker" and len(d["resources"]["table"])==5 else "bad")' <<<"$metrics" 2>&1)"
    check "/metrics counts 4xx" yes "$(python3 -c 'import json,sys; print("yes" if json.load(sys.stdin)["requests"]["status_4xx"]>=5 else "no")' <<<"$metrics")"
fi
stop
check "graceful shutdown message" yes "$(grep -q 'SafeServe stopped' /tmp/safeserve_e2e.log && echo yes || echo no)"

# Concurrent conflicting load: every strategy except naive must answer all.
for mode in banker lockorder detect; do
    start --mode "$mode"
    pids=()
    for i in $(seq 1 6); do
        curl -s -m 8 -o /dev/null -w "%{http_code}\n" -X POST -d x "$BASE/upload" >> /tmp/safeserve_e2e_codes &
        pids+=($!)
        curl -s -m 8 -o /dev/null -w "%{http_code}\n" -X POST "$BASE/refresh" >> /tmp/safeserve_e2e_codes &
        pids+=($!)
    done
    wait "${pids[@]}"
    answered=$(grep -cE '^(200|503)$' /tmp/safeserve_e2e_codes)
    rm -f /tmp/safeserve_e2e_codes
    check "$mode: 12 conflicting requests all answered" 12 "$answered"
    stop
done

# And naive must reproduce the deadlock: some requests are never answered.
start --mode naive
pids=()
for i in $(seq 1 6); do
    curl -s -m 3 -o /dev/null -w "%{http_code}\n" -X POST -d x "$BASE/upload" >> /tmp/safeserve_e2e_codes &
    pids+=($!)
    curl -s -m 3 -o /dev/null -w "%{http_code}\n" -X POST "$BASE/refresh" >> /tmp/safeserve_e2e_codes &
    pids+=($!)
done
wait "${pids[@]}"
check "naive: deadlock reproduced (requests left unanswered)" yes "$(grep -q '^000$' /tmp/safeserve_e2e_codes && echo yes || echo no)"
rm -f /tmp/safeserve_e2e_codes
stop

if [ $fail -ne 0 ]; then echo "=== e2e FAILED ==="; exit 1; fi
echo "=== All e2e tests passed ==="
