#!/bin/bash
# Launches python-ismrmrd-server in the background on TCP 9004, then runs the
# HTTP->TCP shim in the foreground on 9002. The shim uses the port the
# umbrella demo and marshal clients already expect (9002); python-ismrmrd-server
# is moved to 9004 to free that port for the shim.
set -euo pipefail

echo "[entrypoint] starting python-ismrmrd-server on 0.0.0.0:9004"
python3 /opt/python-ismrmrd-server/main.py \
    -H 0.0.0.0 -p 9004 \
    -l /tmp/ismrmrd-server.log \
    -d /tmp/savedata &
RECON_PID=$!

# Give the TCP server a moment to bind before the shim starts accepting HTTP.
sleep 1

echo "[entrypoint] starting shim on 0.0.0.0:9002"
python3 /opt/recon-shim/shim.py &
SHIM_PID=$!

# Forward SIGTERM/SIGINT to both children.
trap 'kill -TERM "$RECON_PID" "$SHIM_PID" 2>/dev/null || true' TERM INT

wait -n "$RECON_PID" "$SHIM_PID"
# If either exits, kill the other and propagate the exit code.
kill -TERM "$RECON_PID" "$SHIM_PID" 2>/dev/null || true
wait
