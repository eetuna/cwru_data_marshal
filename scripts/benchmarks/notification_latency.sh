#!/bin/bash
# scripts/notification_latency.sh
set -e

PORT=8085
WS_PORT=8095
DATA_DIR="./data_notify_bench"

cleanup() {
    pkill -f "build/marshal" || true
    rm -rf "$DATA_DIR"
}
trap cleanup EXIT

mkdir -p "$DATA_DIR"

./build/marshal --http 127.0.0.1:$PORT --ws 127.0.0.1:$WS_PORT --data "$DATA_DIR" --sink mrd > /dev/null 2>&1 &
sleep 2

run_bench() {
    python3 -u - <<EOF
import asyncio
import json
import websockets
import time
import requests
import os

async def bench():
    uri = "ws://127.0.0.1:$WS_PORT/ws"
    async with websockets.connect(uri) as ws:
        await ws.send(json.dumps({"subscribe": "mrd"}))
        await ws.recv() # ack
        
        # We measure time from the moment POST returns to the moment WS arrives
        # This measures the server's fan-out overhead
        
        url = "http://127.0.0.1:$PORT/v1/mrd/ingest"
        data = b"dummy data"
        
        # Start timer AFTER the post completes
        resp = requests.post(url, data=data)
        post_done = time.perf_counter()
        
        msg = await ws.recv()
        ws_done = time.perf_counter()
        
        latency = (ws_done - post_done) * 1000
        print(f"{latency:.2f}")

asyncio.run(bench())
EOF
}

echo "================================================================"
echo "          WEBSOCKET NOTIFICATION LATENCY (ms)                   "
echo "================================================================"
echo "Time from HTTP Ingest Completion to WebSocket Message Arrival"
echo "----------------------------------------------------------------"

for i in {1..5}; do
    L=$(run_bench)
    echo "Trial $i: $L ms"
done
