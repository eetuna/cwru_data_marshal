#!/bin/bash
# End-to-end mode test: live/dump x k-space/image, through the real stack.
#
#   ./scripts/test-modes.sh
#
# Rebuild first if you changed code:
#   ./scripts/build-client-images.sh      # builds the 5 images incl. fire-python
# (marshal code lives in .worktrees/mri_data_marshal; webgl/robot in
#  .worktrees/robot_data_marshal — the build script builds from those worktrees.)
#
# HTTP checks run via `docker exec ... curl` so this works from any shell
# (published ports land on the docker host, not necessarily your $PWD host).
set -u
cd "$(dirname "$0")/.."
NET=cwru-demo-net
DATA="$PWD/session-data"
FP=fire-python:latest
pass=0; fail=0
chk(){ if [ "$2" = "$3" ]; then echo "  PASS $1 ($2)"; pass=$((pass+1)); else echo "  FAIL $1 (got $2 want $3)"; fail=$((fail+1)); fi; }
mexec(){ docker exec cwru-mri-marshal "$@"; }
code(){ mexec curl -s -o /dev/null -w "%{http_code}" localhost:8080/image/latest; }
exists(){ docker run --rm -v "$DATA:/d" $FP sh -c "ls /d/$1 >/dev/null 2>&1 && echo yes || echo no"; }
# Which lane the latest snapshot came from (from_reconstruction|from_scanner).
# Read via HTTP so it works wherever the snapshot lives (RAM dir or disk).
lane(){ mexec curl -s localhost:8080/image/latest | grep -o 'from_[a-z]*' | head -1; }
# Save the latest snapshot bytes into session-data (input for the image pushes).
snap(){ mexec curl -s -o "/session-data/$1" localhost:8080/image/latest.h5; }
push_kspace(){ docker run --rm --network $NET -v "$DATA:/data" $FP \
  python3 client.py -c invertcontrast -o /data/out_k.h5 --address mri-marshal --port 9100 /data/phantom.h5 >/dev/null 2>&1; }
push_image(){ docker run --rm --network $NET -v "$DATA:/data" $FP \
  python3 client.py -o /data/out_i.h5 --address mri-marshal --port 9100 "$1" >/dev/null 2>&1; }

mkdir -p session-data

echo "== bring up stack (live) with bundled test recon =="
docker compose --profile test-recon up -d >/dev/null 2>&1
for i in $(seq 1 30); do [ "$(mexec curl -s -o /dev/null -w '%{http_code}' localhost:8080/health 2>/dev/null)" = "200" ] && break; sleep 2; done

echo "== generate phantom (k-space) =="
docker run --rm -v "$DATA:/data" $FP python3 generate_cartesian_shepp_logan_dataset.py -o /data/phantom.h5 >/dev/null 2>&1

echo "[1] LIVE + k-space (recon reconstructs)"
push_kspace
chk "/image/latest=200" "$(code)" "200"
chk "latest lane = recon" "$(lane)" "from_reconstruction"
snap snap_recon.h5   # keep the recon image as input for [2]

echo "[2] LIVE + image (bypass recon)"
push_image /data/snap_recon.h5
chk "/image/latest=200" "$(code)" "200"
chk "latest lane = scanner" "$(lane)" "from_scanner"
snap snap_scanner.h5 # keep the scanner-lane image as input for [4]

echo "== switch marshal to DUMP =="
MARSHAL_DUMP=--dump docker compose --profile test-recon up -d --force-recreate mri-marshal >/dev/null 2>&1
for i in $(seq 1 30); do [ "$(mexec curl -s -o /dev/null -w '%{http_code}' localhost:8080/health 2>/dev/null)" = "200" ] && break; sleep 2; done

echo "[3] DUMP + k-space"
push_kspace
chk "/image/latest=404" "$(code)" "404"
chk "dump recon archive"   "$(exists 'dump/from_reconstruction/scan_*.h5')" "yes"
chk "dump scanner archive" "$(exists 'dump/from_scanner/scan_*.h5')" "yes"

echo "[4] DUMP + image"
push_image /data/snap_scanner.h5
chk "/image/latest=404" "$(code)" "404"
chk "dump scanner archive (image)" "$(exists 'dump/from_scanner/scan_*.h5')" "yes"

echo "== restore LIVE mode =="
docker compose --profile test-recon up -d --force-recreate mri-marshal >/dev/null 2>&1

echo ""
echo "== RESULT: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
