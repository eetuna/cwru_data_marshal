#!/bin/bash
# scripts/get_robot_marshal.sh
# Pulls the specialized Robot Data Marshal from its branch and builds it here.

set -e

TEMP_DIR="./robot_marshal_tmp_src"

echo "[*] Fetching latest upstream/robot-data-marshal..."
git fetch upstream

echo "[*] Extracting Robot Data Marshal from branch 'upstream/robot-data-marshal'..."

mkdir -p "$TEMP_DIR"
git show upstream/robot-data-marshal:server.cpp > "$TEMP_DIR/server.cpp"
git show upstream/robot-data-marshal:httplib.h > "$TEMP_DIR/httplib.h"
git show upstream/robot-data-marshal:json.hpp > "$TEMP_DIR/json.hpp"
git show upstream/robot-data-marshal:circularBuffer.hpp > "$TEMP_DIR/circularBuffer.hpp"

echo "[*] Compiling Robot Marshal..."
# Use -I so the compiler can find the extracted headers
g++ -I "$TEMP_DIR" "$TEMP_DIR/server.cpp" -o ./build/robot_marshal_demo -lpthread

echo ""
echo "========================================================="
echo "SUCCESS: Real Robot Marshal built at ./build/robot_marshal_demo"
echo "You can now run the demo with authentic specialized code."
echo "========================================================="