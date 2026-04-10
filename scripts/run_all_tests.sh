#!/bin/bash
# scripts/run_all_tests.sh — Run all marshal tests (unit + integration)

set -e
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$PROJECT_DIR/build"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PASS=0
FAIL=0

run_test() {
    local name="$1"
    local cmd="$2"
    echo -n "Running $name... "
    if eval "$cmd" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${NC}"
        ((PASS++))
    else
        echo -e "${RED}FAIL${NC}"
        ((FAIL++))
    fi
}

echo "=== Unit Tests ==="
run_test "unit_pose"         "$BUILD_DIR/unit_pose"
run_test "test_mrd_sink"     "$BUILD_DIR/test_mrd_sink"
run_test "unit_http_handlers" "$BUILD_DIR/unit_http_handlers"
run_test "it_http"           "$BUILD_DIR/it_http"
run_test "test_ws_client"    "$BUILD_DIR/test_ws_client"

echo ""
echo "=== Integration Tests ==="
run_test "T1-T4 integration" "python3 $PROJECT_DIR/tests/integration/test_marshal_integration.py"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
