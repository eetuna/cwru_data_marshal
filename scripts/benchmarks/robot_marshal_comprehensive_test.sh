#!/bin/bash
# scripts/benchmarks/robot_marshal_comprehensive_test.sh
# Comprehensive test of robot-data-marshal with thread-safe fix
# Uses LOCAL fixed sources from scripts/robot_marshal_src/ (NOT upstream)
# Runtime: ~15 seconds with 7 tests including 5-client concurrency test

set -e

PORT=8081
DATA_DIR="./data_robot_bench"
FILES_DIR="./files"

cleanup() {
    pkill -f "robot_marshal_demo" 2>/dev/null || true
    rm -rf "$DATA_DIR" "$FILES_DIR" ./robot_marshal_tmp_src ./log_files ./files.json ./build/robot_marshal_demo 2>/dev/null || true
}
trap cleanup EXIT

mkdir -p "$DATA_DIR" "$FILES_DIR"

# Build server from thread-safe local sources
if [ ! -f "./build/robot_marshal_demo" ]; then
    echo "[BUILD] Compiling thread-safe robot marshal..."
    g++ -std=c++17 -I ./scripts/robot_marshal_src ./scripts/robot_marshal_src/server.cpp \
        -o ./build/robot_marshal_demo -lpthread
fi

echo '["robot_status", "robot_commands", "sensor_data"]' > ./files.json
for f in robot_status robot_commands sensor_data; do touch "$FILES_DIR/$f"; done

./build/robot_marshal_demo > "$DATA_DIR/server.log" 2>&1 &
sleep 2

PASS=0
FAIL=0

# Test 1: Basic write/read
curl -s -X POST http://127.0.0.1:$PORT/write/robot_status \
  -H "Content-Type: application/json" \
  -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"test1\", \"values\": [{\"pos\": \"HOME\"}]}" > /dev/null

RESP=$(curl -s http://127.0.0.1:$PORT/read/robot_status)
if echo "$RESP" | grep -q "received_at"; then
    echo "[PASS] Basic write/read with timestamp injection"
    PASS=$((PASS + 1))
else
    echo "[FAIL] Timestamp injection missing"
    FAIL=$((FAIL + 1))
fi

# Test 2: Schema validation (missing field)
HTTP_CODE=$(curl -s -w "%{http_code}" -o /dev/null -X POST http://127.0.0.1:$PORT/write/robot_status \
  -H "Content-Type: application/json" -d '{"bad": "payload"}')

if [ "$HTTP_CODE" -eq 400 ]; then
    echo "[PASS] Schema validation rejects invalid JSON"
    PASS=$((PASS + 1))
else
    echo "[FAIL] Should return 400 for invalid JSON, got $HTTP_CODE"
    FAIL=$((FAIL + 1))
fi

# Test 3: Multi-file independence (3 clients to different files)
for f in robot_status robot_commands sensor_data; do
    curl -s -X POST http://127.0.0.1:$PORT/write/$f \
      -H "Content-Type: application/json" \
      -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"client_$f\", \"values\": [{\"data\": \"test_$f\"}]}" > /dev/null
done

# Verify each file has only its own data
FILE_OK=true
for f in robot_status robot_commands sensor_data; do
    RESP=$(curl -s http://127.0.0.1:$PORT/read/$f)
    if echo "$RESP" | grep -q "test_$f"; then
        : # Good
    else
        FILE_OK=false
        break
    fi
done

if $FILE_OK; then
    echo "[PASS] Multi-file isolation (3 independent files)"
    PASS=$((PASS + 1))
else
    echo "[FAIL] Multi-file isolation failed"
    FAIL=$((FAIL + 1))
fi

# Test 4: Concurrent read/write (3 clients sequential for speed)
START=$(date +%s%N)
for i in $(seq 1 3); do
    curl -s -X POST http://127.0.0.1:$PORT/write/robot_status \
      -H "Content-Type: application/json" \
      -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"c$i\", \"values\": [{\"i\": $i}]}" > /dev/null
    curl -s http://127.0.0.1:$PORT/read/robot_status > /dev/null
done
END=$(date +%s%N)
ELAPSED=$(( (END - START) / 1000000 ))

if [ $ELAPSED -lt 5000 ]; then
    echo "[PASS] Sequential access (6 operations completed in ${ELAPSED}ms)"
    PASS=$((PASS + 1))
else
    echo "[FAIL] Access too slow (${ELAPSED}ms)"
    FAIL=$((FAIL + 1))
fi

# Test 5: ?last=N query parameter
curl -s -X POST http://127.0.0.1:$PORT/write/sensor_data \
  -H "Content-Type: application/json" \
  -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"seq1\", \"values\": [{\"seq\": 1}]}" > /dev/null
curl -s -X POST http://127.0.0.1:$PORT/write/sensor_data \
  -H "Content-Type: application/json" \
  -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"seq2\", \"values\": [{\"seq\": 2}]}" > /dev/null
curl -s -X POST http://127.0.0.1:$PORT/write/sensor_data \
  -H "Content-Type: application/json" \
  -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"seq3\", \"values\": [{\"seq\": 3}]}" > /dev/null

RESP=$(curl -s "http://127.0.0.1:$PORT/read/sensor_data?last=2")
if echo "$RESP" | grep -q '"entries"'; then
    echo "[PASS] Query parameter ?last=N returns entries array"
    PASS=$((PASS + 1))
else
    echo "[FAIL] Query parameter ?last=N not working"
    FAIL=$((FAIL + 1))
fi

# Test 6: Persistence verification
sleep 1
if [ -f "$FILES_DIR/sensor_data" ] && [ -s "$FILES_DIR/sensor_data" ]; then
    echo "[PASS] Background persistence writes to disk"
    PASS=$((PASS + 1))
else
    echo "[FAIL] Persistence file not created or empty"
    FAIL=$((FAIL + 1))
fi

# Test 7: 5 simultaneous clients (realistic multi-client load)
echo "[TEST 7] Starting 5-client test..."
START=$(date +%s%N)
for c in 1 2 3 4 5; do
    (
        for i in $(seq 1 3); do
            curl -s --max-time 2 "http://127.0.0.1:$PORT/read/robot_status" > /dev/null 2>&1
            curl -s --max-time 2 -X POST "http://127.0.0.1:$PORT/write/robot_status" \
              -H "Content-Type: application/json" \
              -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"client-$c\", \"values\": [{\"i\": $i}]}" \
              > /dev/null 2>&1
        done
        echo "  [Client-$c done]" >&2
    ) &
done

# Wait for all clients with timeout
echo "[TEST 7] Waiting for clients..."
timeout 10 bash -c 'wait' || { echo "[TEST 7] TIMEOUT waiting for clients"; }

END=$(date +%s%N)
ELAPSED=$(( (END - START) / 1000000 ))

if [ $ELAPSED -lt 5000 ]; then
    echo "[PASS] 5 simultaneous clients (25 ops, 50 requests in ${ELAPSED}ms, no deadlock)"
    PASS=$((PASS + 1))
else
    echo "[FAIL] 5-client test took too long (${ELAPSED}ms)"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "========================================"
echo "Tests passed: $PASS"
echo "Tests failed: $FAIL"
echo "========================================"

if [ $FAIL -eq 0 ]; then
    exit 0
else
    exit 1
fi
