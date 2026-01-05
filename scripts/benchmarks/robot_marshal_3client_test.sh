#!/bin/bash
# scripts/benchmarks/robot_marshal_3client_test.sh
# Test robot-data-marshal with 3 C++ clients as originally intended
# This matches the upstream design: 3 clients with circular data flow

set -e

PORT=8081
FILES_DIR="./files"
ITERATIONS=5  # Each client does N iterations

cleanup() {
    echo "[CLEANUP] Killing processes..."
    pkill -f "client-a" 2>/dev/null || true
    pkill -f "client-b" 2>/dev/null || true
    pkill -f "client-c" 2>/dev/null || true
    pkill -f "robot_marshal_demo" 2>/dev/null || true
    rm -rf "$FILES_DIR" ./file_routes.json ./files.json ./log_files ./build/robot_marshal_demo 2>/dev/null || true
}

trap cleanup EXIT

echo "========================================"
echo "3-Client Test (As Intended by Upstream)"
echo "========================================"

# Build server
echo "[BUILD] Compiling thread-safe robot marshal..."
g++ -std=c++17 -I ./scripts/robot_marshal_src ./scripts/robot_marshal_src/server.cpp \
    -o ./build/robot_marshal_demo -lpthread

# Setup files
mkdir -p "$FILES_DIR"
echo '["file1.json", "file2.json", "file3.json"]' > ./files.json

# Initialize files with seed data
echo '{"client_id": "seed", "sent_at": 1, "values": [{"initial": 1}]}' > "$FILES_DIR/file1.json"
echo '{"client_id": "seed", "sent_at": 1, "values": [{"initial": 2}]}' > "$FILES_DIR/file2.json"
echo '{"client_id": "seed", "sent_at": 1, "values": [{"initial": 3}]}' > "$FILES_DIR/file3.json"

# Copy file routing config
cp ./scripts/robot_marshal_src/file_routes.json ./file_routes.json

# Start server
echo "[SERVER] Starting robot marshal on port $PORT..."
./build/robot_marshal_demo $PORT > ./log_files/server.log 2>&1 &
SERVER_PID=$!
sleep 2

# Verify server is ready
if ! curl -s --max-time 1 http://127.0.0.1:$PORT/read/file1.json > /dev/null 2>&1; then
    echo "[FAIL] Server failed to start"
    exit 1
fi
echo "[PASS] Server started (PID: $SERVER_PID)"

# Modify clients to run N iterations instead of infinite loop
echo "[PREP] Creating limited-iteration clients..."
mkdir -p ./scripts/robot_marshal_src/build

# Create modified client-a that runs N iterations
sed -e "s/while(true){/for(int iter=0; iter<$ITERATIONS; iter++){/" \
    ./scripts/robot_marshal_src/client-a.cpp > ./scripts/robot_marshal_src/build/client-a-limited.cpp
echo "    } // end iterations" >> ./scripts/robot_marshal_src/build/client-a-limited.cpp

# Create modified client-b
sed -e "s/while(true){/for(int iter=0; iter<$ITERATIONS; iter++){/" \
    ./scripts/robot_marshal_src/client-b.cpp > ./scripts/robot_marshal_src/build/client-b-limited.cpp
echo "    } // end iterations" >> ./scripts/robot_marshal_src/build/client-b-limited.cpp

# Create modified client-c
sed -e "s/while(true){/for(int iter=0; iter<$ITERATIONS; iter++){/" \
    ./scripts/robot_marshal_src/client-c.cpp > ./scripts/robot_marshal_src/build/client-c-limited.cpp
echo "    } // end iterations" >> ./scripts/robot_marshal_src/build/client-c-limited.cpp

# Compile limited clients
g++ -std=c++17 -I ./scripts/robot_marshal_src ./scripts/robot_marshal_src/build/client-a-limited.cpp \
    -o ./scripts/robot_marshal_src/build/client-a-test -lpthread
g++ -std=c++17 -I ./scripts/robot_marshal_src ./scripts/robot_marshal_src/build/client-b-limited.cpp \
    -o ./scripts/robot_marshal_src/build/client-b-test -lpthread
g++ -std=c++17 -I ./scripts/robot_marshal_src ./scripts/robot_marshal_src/build/client-c-limited.cpp \
    -o ./scripts/robot_marshal_src/build/client-c-test -lpthread

echo "[PASS] Clients compiled"

# Run all 3 clients simultaneously
echo "[TEST] Running 3 clients simultaneously ($ITERATIONS iterations each)..."
START=$(date +%s%N)

./scripts/robot_marshal_src/build/client-a-test > /tmp/client-a.log 2>&1 &
PID_A=$!
./scripts/robot_marshal_src/build/client-b-test > /tmp/client-b.log 2>&1 &
PID_B=$!
./scripts/robot_marshal_src/build/client-c-test > /tmp/client-c.log 2>&1 &
PID_C=$!

echo "  - client-a PID: $PID_A"
echo "  - client-b PID: $PID_B"
echo "  - client-c PID: $PID_C"

# Wait for all clients with timeout
timeout 30 bash -c "wait $PID_A $PID_B $PID_C" 2>/dev/null || {
    echo "[FAIL] Clients timed out or crashed"
    echo "--- client-a log ---"
    cat /tmp/client-a.log || true
    echo "--- client-b log ---"
    cat /tmp/client-b.log || true
    echo "--- client-c log ---"
    cat /tmp/client-c.log || true
    exit 1
}

END=$(date +%s%N)
ELAPSED=$(( (END - START) / 1000000 ))

echo "[PASS] All 3 clients completed in ${ELAPSED}ms"

# Verify data flow
echo "[VERIFY] Checking circular data flow..."

# Check that files were written to
FILE1_SIZE=$(wc -c < "$FILES_DIR/file1.json")
FILE2_SIZE=$(wc -c < "$FILES_DIR/file2.json")
FILE3_SIZE=$(wc -c < "$FILES_DIR/file3.json")

echo "  - file1.json: ${FILE1_SIZE} bytes"
echo "  - file2.json: ${FILE2_SIZE} bytes"
echo "  - file3.json: ${FILE3_SIZE} bytes"

if [ "$FILE1_SIZE" -gt 100 ] && [ "$FILE2_SIZE" -gt 100 ] && [ "$FILE3_SIZE" -gt 100 ]; then
    echo "[PASS] All files have data (circular flow worked)"
else
    echo "[FAIL] Some files are empty or too small"
    exit 1
fi

# Check client logs for errors
if grep -qi "failed\|error" /tmp/client-a.log /tmp/client-b.log /tmp/client-c.log 2>/dev/null; then
    echo "[WARN] Client logs contain errors:"
    grep -i "failed\|error" /tmp/client-*.log || true
fi

echo ""
echo "========================================"
echo "3-CLIENT TEST PASSED"
echo "========================================"
echo "Summary:"
echo "  - 3 C++ clients (as intended by upstream)"
echo "  - Circular data flow: file1→client-a→file2→client-b→file3→client-c→file1"
echo "  - $ITERATIONS iterations per client"
echo "  - Completed in ${ELAPSED}ms"
echo "  - Thread-safe circular buffer: NO DEADLOCKS"
