#!/bin/bash
# scripts/read_latency_bench.sh
set -e

PORT=8085
WS_PORT=8095
DATA_DIR="./data_read_bench"

cleanup() {
    pkill -f "build/marshal" || true
    rm -rf "$DATA_DIR"
}
trap cleanup EXIT

mkdir -p "$DATA_DIR"

./build/marshal --http 127.0.0.1:$PORT --ws 127.0.0.1:$WS_PORT --data "$DATA_DIR" --sink mrd > /dev/null 2>&1 &
sleep 2

run_read_bench() {
    local nx=$1
    local nz=$2
    
    python3 -u - <<EOF
import asyncio
import json
import websockets
import time
import h5py
import os
import sys

async def bench():
    uri = "ws://127.0.0.1:$WS_PORT/ws"
    try:
        async with websockets.connect(uri) as ws:
            await ws.send(json.dumps({"subscribe": "mrd"}))
            await ws.recv()
            
            start_trigger = time.perf_counter()
            os.system(f"./build/image_streamer --http http://127.0.0.1:$PORT --nx $nx --ny $nx --slices $nz --frames 1 --dt-ms 0 > /dev/null")
            
            msg = await ws.recv()
            recv_time = time.perf_counter()
            
            data = json.loads(msg)
            if data.get("type") == "mrd":
                path = data["path"]
                
                # Retry loop for SWMR synchronization
                val = None
                for _ in range(100):
                    try:
                        with h5py.File(path, 'r', libver='latest', swmr=True) as f:
                            dset = f['/images/data']
                            dset.refresh()
                            if dset.shape[0] > 0:
                                val = dset[0, 0, 0, 0, 0]
                                break
                    except:
                        pass
                    time.sleep(0.01)
                
                read_done_time = time.perf_counter()
                
                if val is not None:
                    i2n = (recv_time - start_trigger) * 1000
                    n2r = (read_done_time - recv_time) * 1000
                    total = (read_done_time - start_trigger) * 1000
                    print(f"{i2n}|{n2r}|{total}")
                else:
                    print("ERROR: Timeout waiting for SWMR data", file=sys.stderr)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)

asyncio.run(bench())
EOF
}

echo "================================================================"
echo "          CLIENT READ LATENCY BENCHMARK (ms)                    "
echo "================================================================"
printf "| %-7s | %-6s | %-12s | %-12s | %-12s |\n" "Res" "Slices" "Ingest->WS" "WS->Read" "Total E2E"
echo "----------------------------------------------------------------"

for res in 128 256; do
    for slices in 5 20; do
        RESULT=$(run_read_bench $res $slices)
        if [ ! -z "$RESULT" ]; then
            I2N=$(echo "$RESULT" | cut -d'|' -f1)
            N2R=$(echo "$RESULT" | cut -d'|' -f2)
            E2E=$(echo "$RESULT" | cut -d'|' -f3)
            printf "| %-7s | %-6s | %-12s | %-12s | %-12s |\n" "${res}x${res}" "$slices" "${I2N} ms" "${N2R} ms" "${E2E} ms"
        else
            echo "Error in case ${res}x${res}"
        fi
    done
done