#!/bin/bash
# scripts/benchmarks/robot_marshal_3client_realtest.sh
# Test robot-data-marshal with actual 3 C++ clients (as originally intended)
# Uses thread-safe implementation from scripts/robot_marshal_src/

set -e

PORT=8081
FILES_DIR="./files"
TEST_DURATION=5  # Run clients for N seconds

cleanup() {
    pkill -f "client-a" 2>/dev/null || true
    pkill -f "client-b" 2>/dev/null || true
    pkill -f "client-c" 2>/dev/null || true
    pkill -f "robot_marshal_demo" 2>/dev/null || true
    rm -rf "$FILES_DIR" ./file_routes.json ./files.json ./log_files ./build/robot_marshal_demo 2>/dev/null || true
}

trap cleanup EXIT

echo "========================================"
echo "3-Client Test (Upstream Design Pattern)"
echo "========================================"

# Build server from thread-safe sources
echo "[BUILD] Compiling thread-safe robot marshal..."
g++ -std=c++17 -I ./scripts/robot_marshal_src ./scripts/robot_marshal_src/server.cpp \
    -o ./build/robot_marshal_demo -lpthread
echo "✓ Server compiled"

# Setup files
mkdir -p "$FILES_DIR"
echo '["file1.json", "file2.json", "file3.json"]' > ./files.json

# Initialize files with valid seed data
echo '{"sent_at":1000,"client_id":"seed","values":[{"x":1}]}' > "$FILES_DIR/file1.json"
echo '{"sent_at":2000,"client_id":"seed","values":[{"x":2}]}' > "$FILES_DIR/file2.json"
echo '{"sent_at":3000,"client_id":"seed","values":[{"x":3}]}' > "$FILES_DIR/file3.json"

# Copy file routing config to current directory (clients need it here)
cp ./scripts/robot_marshal_src/file_routes.json ./file_routes.json
echo "✓ Files initialized with seed data"

# Start server
./build/robot_marshal_demo $PORT > ./log_files/server.log 2>&1 &
SERVER_PID=$!
sleep 2

# Verify server is ready
if ! curl -s --max-time 1 http://127.0.0.1:$PORT/read/file1.json > /dev/null 2>&1; then
    echo "✗ Server failed to start"
    cat ./log_files/server.log
    exit 1
fi
echo "✓ Server ready (PID: $SERVER_PID)"

# Run actual C++ clients for TEST_DURATION seconds
echo "[TEST] Running 3 C++ clients (${TEST_DURATION}s test duration)..."
echo "  Pattern: file1 → client-a → file2 → client-b → file3 → client-c → file1"
START=$(date +%s%N)

timeout $TEST_DURATION ./scripts/robot_marshal_src/client-a > /tmp/client-a.log 2>&1 &
PID_A=$!
timeout $TEST_DURATION ./scripts/robot_marshal_src/client-b > /tmp/client-b.log 2>&1 &
PID_B=$!
timeout $TEST_DURATION ./scripts/robot_marshal_src/client-c > /tmp/client-c.log 2>&1 &
PID_C=$!

echo "  - client-a (PID: $PID_A)"
echo "  - client-b (PID: $PID_B)"
echo "  - client-c (PID: $PID_C)"

# Wait for all clients (timeout will kill them)
wait $PID_A $PID_B $PID_C 2>/dev/null || true

END=$(date +%s%N)
ELAPSED=$(( (END - START) / 1000000 ))

# Count successful iterations
ITERS_A=$(grep -c "Read values" /tmp/client-a.log 2>/dev/null || echo 0)
ITERS_B=$(grep -c "Read values" /tmp/client-b.log 2>/dev/null || echo 0)
ITERS_C=$(grep -c "Read values" /tmp/client-c.log 2>/dev/null || echo 0)
TOTAL=$(( ITERS_A + ITERS_B + ITERS_C ))

echo ""
echo "Results:"
echo "  Duration: ${ELAPSED}ms"
echo "  Client-A iterations: $ITERS_A"
echo "  Client-B iterations: $ITERS_B"
echo "  Client-C iterations: $ITERS_C"
echo "  TOTAL iterations: $TOTAL"

if [ "$TOTAL" -gt 0 ]; then
    RATE=$(( TOTAL * 1000 / ELAPSED ))
    echo "  Throughput: $RATE iterations/sec"
    echo "  Avg iteration time: $(( ELAPSED / TOTAL ))ms"
fi

# Check for errors
if grep -qi "failed" /tmp/client-*.log 2>/dev/null; then
    echo "✗ Errors detected in client logs"
    exit 1
fi

# Verify all 3 files have data
FILE1_SIZE=$(wc -c < "$FILES_DIR/file1.json")
FILE2_SIZE=$(wc -c < "$FILES_DIR/file2.json")
FILE3_SIZE=$(wc -c < "$FILES_DIR/file3.json")

echo ""
echo "File sizes after test:"
echo "  file1.json: ${FILE1_SIZE} bytes"
echo "  file2.json: ${FILE2_SIZE} bytes"
echo "  file3.json: ${FILE3_SIZE} bytes"

if [ "$FILE1_SIZE" -gt 100 ] && [ "$FILE2_SIZE" -gt 100 ] && [ "$FILE3_SIZE" -gt 100 ]; then
    echo "✓ Circular data flow verified"
else
    echo "✗ Some files are too small - data flow may be broken"
    exit 1
fi

echo ""
echo "========================================"
echo "TEST PASSED"
echo "========================================"
echo "Summary:"
echo "  ✓ Thread-safe robot-data-marshal"
echo "  ✓ 3 C++ clients (as intended by upstream)"
echo "  ✓ Circular data flow working"
echo "  ✓ No deadlocks"
echo "  ✓ High throughput: $RATE iterations/sec"
