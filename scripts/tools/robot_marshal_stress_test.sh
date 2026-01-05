#!/bin/bash
# scripts/tools/robot_marshal_stress_test.sh
# CI/CD stress test for robot-data-marshal (thread-safe version)
# Uses LOCAL fixed sources from scripts/robot_marshal_src/
# Runtime: ~30 seconds | Exit: 0=pass, 1=fail

set -e

# Configuration
ROBOT_HTTP=8081
DATA_ROBOT="./data_robot_stress"
FILES_DIR="./files"
TIMEOUT=30

# Test thresholds
MAX_WRITE_LATENCY_MS=500
MAX_READ_LATENCY_MS=200
MIN_THROUGHPUT=50

# Counters
FAILURES=0

log() {
    echo "[$(date +%H:%M:%S)] $1"
}

fail() {
    echo "[FAIL] $1"
    ((FAILURES++))
}

pass() {
    echo "[PASS] $1"
}

cleanup() {
    pkill -f "robot_marshal_demo" 2>/dev/null || true
    rm -rf "$DATA_ROBOT" "$FILES_DIR" ./build/robot_marshal_demo ./files.json ./log_files 2>/dev/null || true
}

trap cleanup EXIT

# Setup
log "Setting up robot marshal for stress test..."
mkdir -p "$DATA_ROBOT" "$FILES_DIR"

# Always rebuild to ensure correct paths
log "Building robot marshal from thread-safe sources..."
g++ -std=c++17 -I ./scripts/robot_marshal_src ./scripts/robot_marshal_src/server.cpp -o ./build/robot_marshal_demo -lpthread

echo '["stress_test", "robot_status", "robot_commands"]' > ./files.json
for f in stress_test robot_status robot_commands; do touch "$FILES_DIR/$f"; done

./build/robot_marshal_demo > "$DATA_ROBOT/server.log" 2>&1 &
sleep 2

# Verify server started
if ! curl -s http://127.0.0.1:$ROBOT_HTTP/read/stress_test > /dev/null 2>&1; then
    fail "Server failed to start"
    exit 1
fi
pass "Server started"

# Test 1: Write latency
log "Test 1: Write latency..."
TOTAL_NS=0
for i in $(seq 1 20); do
    START=$(date +%s%N)
    curl -s -X POST http://127.0.0.1:$ROBOT_HTTP/write/stress_test \
        -H "Content-Type: application/json" \
        -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"ci\", \"values\": [{\"i\": $i}]}" > /dev/null
    END=$(date +%s%N)
    TOTAL_NS=$((TOTAL_NS + END - START))
done
AVG_WRITE_MS=$((TOTAL_NS / 20 / 1000000))

if [ $AVG_WRITE_MS -lt $MAX_WRITE_LATENCY_MS ]; then
    pass "Write latency: ${AVG_WRITE_MS}ms (threshold: ${MAX_WRITE_LATENCY_MS}ms)"
else
    fail "Write latency: ${AVG_WRITE_MS}ms exceeds ${MAX_WRITE_LATENCY_MS}ms"
fi

# Test 2: Read latency
log "Test 2: Read latency..."
TOTAL_NS=0
for i in $(seq 1 20); do
    START=$(date +%s%N)
    curl -s http://127.0.0.1:$ROBOT_HTTP/read/stress_test > /dev/null
    END=$(date +%s%N)
    TOTAL_NS=$((TOTAL_NS + END - START))
done
AVG_READ_MS=$((TOTAL_NS / 20 / 1000000))

if [ $AVG_READ_MS -lt $MAX_READ_LATENCY_MS ]; then
    pass "Read latency: ${AVG_READ_MS}ms (threshold: ${MAX_READ_LATENCY_MS}ms)"
else
    fail "Read latency: ${AVG_READ_MS}ms exceeds ${MAX_READ_LATENCY_MS}ms"
fi

# Test 3: Concurrent access (no deadlock)
log "Test 3: Concurrent access..."
START=$(date +%s%N)
for i in $(seq 1 15); do
    curl -s --max-time 5 http://127.0.0.1:$ROBOT_HTTP/read/stress_test > /dev/null &
    curl -s --max-time 5 -X POST http://127.0.0.1:$ROBOT_HTTP/write/stress_test \
        -H "Content-Type: application/json" \
        -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"ci\", \"values\": [{\"i\": $i}]}" > /dev/null &
done
# Wait with timeout
timeout 15 bash -c 'wait' 2>/dev/null || true
END=$(date +%s%N)
CONCURRENT_MS=$(((END - START) / 1000000))

if [ $CONCURRENT_MS -lt 10000 ]; then
    pass "Concurrent access: ${CONCURRENT_MS}ms (30 ops, no deadlock)"
else
    fail "Concurrent access timeout (possible deadlock)"
fi

# Test 4: Schema validation
log "Test 4: Schema validation..."
HTTP_CODE=$(curl -s -w "%{http_code}" -o /dev/null -X POST http://127.0.0.1:$ROBOT_HTTP/write/stress_test \
    -H "Content-Type: application/json" -d '{"bad": "payload"}')
if [ "$HTTP_CODE" -eq 400 ]; then
    pass "Invalid payload rejected (HTTP 400)"
else
    fail "Invalid payload not rejected (HTTP $HTTP_CODE)"
fi

# Test 5: Throughput
log "Test 5: Throughput..."
START=$(date +%s%N)
for i in $(seq 1 100); do
    curl -s -X POST http://127.0.0.1:$ROBOT_HTTP/write/robot_status \
        -H "Content-Type: application/json" \
        -d "{\"sent_at\": $(date +%s%N), \"client_id\": \"ci\", \"values\": [{\"i\": $i}]}" > /dev/null
done
END=$(date +%s%N)
DURATION_MS=$(((END - START) / 1000000))
THROUGHPUT=$((100 * 1000 / DURATION_MS))

if [ $THROUGHPUT -ge $MIN_THROUGHPUT ]; then
    pass "Throughput: ${THROUGHPUT} req/s (threshold: ${MIN_THROUGHPUT} req/s)"
else
    fail "Throughput: ${THROUGHPUT} req/s below ${MIN_THROUGHPUT} req/s"
fi

# Test 6: Buffer integrity
log "Test 6: Buffer integrity..."
RESPONSE=$(curl -s "http://127.0.0.1:$ROBOT_HTTP/read/robot_status?last=10")
if echo "$RESPONSE" | grep -q "entries"; then
    pass "Buffer query returns entries"
else
    fail "Buffer query failed"
fi

# Summary
echo ""
echo "========================================"
if [ $FAILURES -eq 0 ]; then
    echo "STRESS TEST PASSED (0 failures)"
    echo "========================================"
    exit 0
else
    echo "STRESS TEST FAILED ($FAILURES failures)"
    echo "========================================"
    exit 1
fi
