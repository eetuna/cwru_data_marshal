#!/bin/bash
# Launches python-ismrmrd-server in the background on TCP 9002, then runs the
# HTTP->TCP shim in the foreground on 9003.
set -euo pipefail

echo "[entrypoint] starting python-ismrmrd-server on 0.0.0.0:9002"
python3 /opt/python-ismrmrd-server/main.py \
    -H 0.0.0.0 -p 9002 \
    -l /tmp/ismrmrd-server.log \
    -d /tmp/savedata &
RECON_PID=$!

# Give the TCP server a moment to bind before the shim starts accepting HTTP.
sleep 1

echo "[entrypoint] starting shim on 0.0.0.0:9003"
python3 /opt/recon-shim/shim.py &
SHIM_PID=$!

# Forward SIGTERM/SIGINT to both children.
trap 'kill -TERM "$RECON_PID" "$SHIM_PID" 2>/dev/null || true' TERM INT

wait -n "$RECON_PID" "$SHIM_PID"
# If either exits, kill the other and propagate the exit code.
kill -TERM "$RECON_PID" "$SHIM_PID" 2>/dev/null || true
wait
