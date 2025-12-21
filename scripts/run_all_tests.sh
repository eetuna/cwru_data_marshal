#!/bin/bash
# scripts/run_all_tests.sh
# The master test suite for CWRU Data Marshal.

set -e

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}Starting Comprehensive Test Suite...${NC}"

# 1. Build Verification
echo -e "\n[1/4] Running Build Check..."
cmake -B build -D BUILD_TESTING=ON > /dev/null
cmake --build build > /dev/null
echo -e "${GREEN}Build OK.${NC}"

# 2. Unit & Networking Tests (Catch2)
echo -e "\n[2/4] Running CTest (Unit/Integration)..."
cd build && ctest --output-on-failure
cd ..
echo -e "${GREEN}Unit Tests OK.${NC}"

# 3. System Integration Tests (Task 8)
echo -e "\n[3/4] Running System Integration (Bio/Bridge/Topics)..."
./scripts/tools/verify_system_integration.sh
echo -e "${GREEN}Integration OK.${NC}"

# 4. Stress & Chaos Tests (Task 9)
echo -e "\n[4/4] Running Chaos Stress Test..."
./scripts/benchmarks/chaos_test.sh
echo -e "${GREEN}Stress Test OK.${NC}"

echo -e "\n${GREEN}======================================"
echo "   ALL TESTS PASSED SUCCESSFULLY"
echo -e "======================================${NC}"
