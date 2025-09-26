#!/usr/bin/env bash
set -euo pipefail
BASE="./data/mrd"; mkdir -p "$BASE" "./data/segments"
for i in 1 2 3; do
  test -f "./build/mk_mrd" && ./build/mk_mrd "$BASE/test_$i.h5" || dd if=/dev/urandom of="$BASE/test_$i.h5" bs=1024 count=1
  curl -s -H "Content-Type: application/octet-stream" --data-binary @"$BASE/test_$i.h5" \
    http://localhost:8080/v1/mrd/ingest || true
done
head -n 3 ./data/segments/index.jsonl || true
