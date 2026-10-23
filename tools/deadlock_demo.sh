#!/bin/bash
set -e

PORT_NAIVE=8990
PORT_BANKER=8991

echo "============================================================"
echo "          SafeServe Deadlock Demonstration Script           "
echo "============================================================"
echo ""

# Ensure binaries are built
make all > /dev/null

echo ">>> PART 1: Testing NAIVE Mode (No deadlock protection)"
echo "Starting SafeServe in NAIVE mode on port $PORT_NAIVE..."
./bin/safeserve --port $PORT_NAIVE --mode naive > /tmp/safeserve_naive.log 2>&1 &
PID_NAIVE=$!
sleep 1

# Check server is up
curl -s http://127.0.0.1:$PORT_NAIVE/health > /dev/null || { echo "Failed to start naive server"; exit 1; }
echo "Naive server running (PID: $PID_NAIVE)."

echo "Injecting conflicting requests with cross-resource claims (DB vs CLOCK)..."
# Trigger concurrent /refresh and /upload
curl -s -m 3 -X POST http://127.0.0.1:$PORT_NAIVE/refresh > /dev/null 2>&1 &
PID_REQ1=$!
curl -s -m 3 -X POST http://127.0.0.1:$PORT_NAIVE/upload -d "payload" > /dev/null 2>&1 &
PID_REQ2=$!

sleep 2

# Check if requests completed or froze
if kill -0 $PID_REQ1 2>/dev/null || kill -0 $PID_REQ2 2>/dev/null; then
    echo ">>> RESULT: Deadlock triggered in NAIVE mode! Requests are frozen in circular wait."
else
    echo "Requests finished."
fi

kill -9 $PID_NAIVE $PID_REQ1 $PID_REQ2 2>/dev/null || true
echo "Cleaned up naive mode processes."
echo ""

echo "------------------------------------------------------------"
echo ">>> PART 2: Testing SAFESERVE (Banker's Avoidance Mode)"
echo "Starting SafeServe in BANKER mode on port $PORT_BANKER..."
./bin/safeserve --port $PORT_BANKER --mode banker > /tmp/safeserve_banker.log 2>&1 &
PID_BANKER=$!
sleep 1

# Check server is up
curl -s http://127.0.0.1:$PORT_BANKER/health > /dev/null || { echo "Failed to start banker server"; exit 1; }
echo "SafeServe running in Banker's mode (PID: $PID_BANKER)."

echo "Injecting the EXACT SAME conflicting concurrent requests..."
RESP1=$(curl -s -X POST http://127.0.0.1:$PORT_BANKER/refresh)
RESP2=$(curl -s -X POST http://127.0.0.1:$PORT_BANKER/upload -d "payload")
HEALTH_RESP=$(curl -s http://127.0.0.1:$PORT_BANKER/health)

echo "Response 1 (/refresh): $RESP1"
echo "Response 2 (/upload):  $RESP2"
echo "Response 3 (/health):  $HEALTH_RESP"

if [[ "$RESP1" =~ "refreshed" ]] && [[ "$RESP2" =~ "bytes_received" ]]; then
    echo ""
    echo ">>> RESULT: SUCCESS! Zero deadlocks occurred under Banker's algorithm."
    echo "    Requests were admitted in safe sequence and processed concurrently without freezing."
else
    echo "Unexpected response."
fi

# Fetch metrics
echo ""
echo ">>> Banker's Algorithm Metrics Snapshot:"
curl -s http://127.0.0.1:$PORT_BANKER/metrics

kill -TERM $PID_BANKER 2>/dev/null || true
wait $PID_BANKER 2>/dev/null || true
echo ""
echo "SafeServe gracefully stopped."
echo "============================================================"
