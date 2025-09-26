#!/usr/bin/env bash
set -euo pipefail
mkdir -p build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
pkill -f "/build/marshal" >/dev/null 2>&1 || true
./build/marshal --http 0.0.0.0:8080 --ws 0.0.0.0:8090 --data ./data > ./data/marshal_live.log 2>&1 &
sleep 0.5
./scripts/post_mrds.sh
curl -s -X POST http://localhost:8080/v1/pose/update -H 'Content-Type: application/json' \
  -d '{"p":[0,0,0],"R":[1,0,0,0,1,0,0,0,1],"source":"smoke"}' >/dev/null || true
curl -s http://localhost:8080/v1/pose/current >/dev/null || true
echo "Smoke OK"
