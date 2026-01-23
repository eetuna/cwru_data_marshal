#!/bin/bash
set -e

FILES_DIR="/data/robot_data"
mkdir -p "$FILES_DIR"

if [ -f /opt/robot/files.json ]; then
  python3 - <<'PY'
import json
import os
seed = {"client_id": "seed", "sent_at": 1, "values": [1.0, 2.0, 3.0]}
files_dir = "/data/robot_data"
try:
    files = json.load(open("/opt/robot/files.json"))
except Exception:
    files = []
for name in files:
    path = os.path.join(files_dir, name)
    with open(path, "w") as fh:
        fh.write(json.dumps(seed))
PY
fi

exec /opt/robot/build/robot_marshal_demo 8081
