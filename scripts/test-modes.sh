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
# SESSION_DATA_DIR: set when $PWD is not the path the DOCKER HOST sees for
# this repo (e.g. running from a devcontainer whose /workspaces/... is not
# the host checkout). It is also passed through to docker compose, which
# uses the same variable for the marshal's /session-data bind.
DATA="${SESSION_DATA_DIR:-$PWD/session-data}"
SCRIPTS="${MARSHAL_SCRIPTS_DIR:-$PWD/scripts}"
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
for i in $(seq 1 30); do [ "$(mexec curl -s -o /dev/null -w '%{http_code}' localhost:8080/health 2>/dev/null)" = "200" ] && break; sleep 2; done

echo "[5] SLICE COMMANDS (marshal -> scanner-side slice_agent, channel OFF)"
TGT='{"position":[12.5,-3,40],"read_dir":[1,0,0],"phase_dir":[0,1,0],"slice_dir":[0,0,1]}'
BAD='{"position":[0,0,0],"read_dir":[1,0,0],"phase_dir":[1,0,0],"slice_dir":[0,0,1]}'
LEFT='{"position":[0,0,0],"read_dir":[1,0,0],"phase_dir":[0,1,0],"slice_dir":[0,0,-1]}'
field(){ grep -o "\"$1\":[a-z0-9.-]*"; }
# channel off (no SLICE_AGENT_HOST): accepted + cached, enabled:false, not delivered
chk "slice_target channel off: enabled=false" \
  "$(mexec curl -s -X POST localhost:8080/write/slice_target -d "$TGT" | field enabled)" '"enabled":false'
# invalid prescriptions are rejected before reaching any agent
chk "slice_target bad frame = 400" \
  "$(mexec curl -s -o /dev/null -w '%{http_code}' -X POST localhost:8080/write/slice_target -d "$BAD")" "400"
chk "slice_target left-handed = 400" \
  "$(mexec curl -s -o /dev/null -w '%{http_code}' -X POST localhost:8080/write/slice_target -d "$LEFT")" "400"

echo "[6] SLICE COMMANDS (channel ON -> mock slice_agent)"
# Mock agent on the compose network; the marshal is recreated pointing at it.
# The script is passed inline (python3 -c) rather than bind-mounted so this
# also works from a devcontainer whose $PWD the docker host cannot see.
docker rm -f slice-agent-mock >/dev/null 2>&1
docker run -d --rm --name slice-agent-mock --network $NET $FP \
  python3 -c "$(cat scripts/slice_agent_mock.py)" --port 9270 --timeout 120 >/dev/null
SLICE_AGENT_HOST=slice-agent-mock docker compose --profile test-recon up -d --force-recreate mri-marshal >/dev/null 2>&1
for i in $(seq 1 30); do [ "$(mexec curl -s -o /dev/null -w '%{http_code}' localhost:8080/health 2>/dev/null)" = "200" ] && break; sleep 2; done
# lazy connect: nothing reaches the agent until the first command
chk "no connection before first command" "$(docker logs slice-agent-mock 2>&1 | grep -c CONNECTED)" "0"
# first press from zero: +1 = PgUp -> tz=1 (six numbers start at zero, like slice_control)
chk "nudge +1 delivered" \
  "$(mexec curl -s -X POST localhost:8080/write/file_slice_translation -d '{"client_id":"t","values":[1]}' | field delivered)" '"delivered":true'
sleep 1
chk "agent got tz=1" "$(docker logs slice-agent-mock 2>&1 | grep -q '"tz": 1.0' && echo yes || echo no)" "yes"
# absolute target: six numbers replaced, agent receives tz=40
chk "slice_target delivered" \
  "$(mexec curl -s -X POST localhost:8080/write/slice_target -d "$TGT" | field delivered)" '"delivered":true'
sleep 0.5
chk "agent got tz=40" "$(docker logs slice-agent-mock 2>&1 | grep -q '"tz": 40.0' && echo yes || echo no)" "yes"
# +1 on top of that -> tz 41
mexec curl -s -X POST localhost:8080/write/file_slice_translation -d '{"client_id":"t","values":[1]}' >/dev/null
sleep 0.5
chk "nudge +1 -> agent got tz=41" "$(docker logs slice-agent-mock 2>&1 | grep -q '"tz": 41.0' && echo yes || echo no)" "yes"
# rotation slider: 0.5 rad = 28.65 deg is ADDED to rz (Andrew's E key), positive
mexec curl -s -X POST localhost:8080/write/slice_delta -d '{"rotation_rad":[0,0,0.5]}' >/dev/null
sleep 0.5
chk "rotate 0.5rad -> rz=+28.65" "$(docker logs slice-agent-mock 2>&1 | grep -q '"rz": 28.64' && echo yes || echo no)" "yes"
# graceful shutdown sends 0xDEAD (57005)
docker compose --profile test-recon up -d --force-recreate mri-marshal >/dev/null 2>&1
sleep 2
chk "marshal restart sent QUIT to agent" "$(docker logs slice-agent-mock 2>&1 | grep -q '"flags": 57005' && echo yes || echo no)" "yes"
docker rm -f slice-agent-mock >/dev/null 2>&1
for i in $(seq 1 30); do [ "$(mexec curl -s -o /dev/null -w '%{http_code}' localhost:8080/health 2>/dev/null)" = "200" ] && break; sleep 2; done

echo ""
echo "== RESULT: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
