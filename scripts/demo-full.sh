#!/bin/bash
# Full Docker Demo - All clients running in containers
#
# Usage:
#   ./scripts/demo-full.sh              # Run all (no viz)
#   ./scripts/demo-full.sh --profile mri    # MRI clients only
#   ./scripts/demo-full.sh --profile robot  # Robot clients only
#   ./scripts/demo-full.sh --viz            # Include viz-client (needs X11)

set -e

GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
COMPOSE_FILE="$ROOT_DIR/docker-compose.full.yml"

# Parse arguments
PROFILE="full"
INCLUDE_VIZ=false
for arg in "$@"; do
    case $arg in
        --profile)
            shift
            PROFILE="$1"
            shift
            ;;
        --mri)
            PROFILE="mri"
            ;;
        --robot)
            PROFILE="robot"
            ;;
        --viz)
            INCLUDE_VIZ=true
            ;;
        *)
            ;;
    esac
done

# Adjust profile for viz
if [ "$INCLUDE_VIZ" = true ]; then
    if [ "$PROFILE" = "full" ]; then
        PROFILE="full-viz"
    elif [ "$PROFILE" = "mri" ]; then
        PROFILE="mri-viz"
    fi
fi

echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}   CWRU DATA MARSHAL - FULL DOCKER DEMO                 ${NC}"
echo -e "${CYAN}   Profile: ${PROFILE}                                  ${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo ""

# Check compose file exists
if [ ! -f "$COMPOSE_FILE" ]; then
    echo -e "${RED}ERROR: $COMPOSE_FILE not found${NC}"
    exit 1
fi

# Build images
echo -e "${YELLOW}Building images...${NC}"
docker compose -f "$COMPOSE_FILE" --profile "$PROFILE" build

# Start marshals first
echo -e "${YELLOW}Starting marshals...${NC}"
docker compose -f "$COMPOSE_FILE" up -d mri-marshal robot-marshal

# Wait for marshals to be healthy
echo -e "${YELLOW}Waiting for marshals to be healthy...${NC}"
for i in {1..30}; do
    MRI_HEALTHY=$(docker inspect --format='{{.State.Health.Status}}' cwru-mri-marshal 2>/dev/null || echo "starting")
    ROBOT_HEALTHY=$(docker inspect --format='{{.State.Health.Status}}' cwru-robot-marshal 2>/dev/null || echo "starting")

    if [ "$MRI_HEALTHY" = "healthy" ] && [ "$ROBOT_HEALTHY" = "healthy" ]; then
        break
    fi

    echo -n "."
    sleep 1
done
echo ""

# Verify marshals
if curl -sf --max-time 2 http://localhost:8080/health > /dev/null 2>&1; then
    echo -e "${GREEN}✓ MRI Marshal healthy (localhost:8080)${NC}"
else
    echo -e "${RED}✗ MRI Marshal not responding${NC}"
fi

if curl -sf --max-time 2 http://localhost:8081/ > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Robot Marshal healthy (localhost:8081)${NC}"
else
    echo -e "${YELLOW}⚠ Robot Marshal not responding (may be normal)${NC}"
fi

echo ""

# Start clients with profile
echo -e "${YELLOW}Starting clients (profile: $PROFILE)...${NC}"
docker compose -f "$COMPOSE_FILE" --profile "$PROFILE" up -d

echo ""
echo -e "${GREEN}═══════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}   DEMO RUNNING - All services started                  ${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════${NC}"
echo ""
echo -e "View logs: ${CYAN}docker compose -f docker-compose.full.yml logs -f${NC}"
echo -e "Stop:      ${CYAN}docker compose -f docker-compose.full.yml down${NC}"
echo ""
echo -e "External access:"
echo -e "  MRI Marshal:   ${CYAN}http://localhost:8080${NC}"
echo -e "  Robot Marshal: ${CYAN}http://localhost:8081${NC}"
echo -e "  MRI WebSocket: ${CYAN}ws://localhost:8090/ws${NC}"
echo ""

# Show running containers
echo -e "${YELLOW}Running containers:${NC}"
docker compose -f "$COMPOSE_FILE" ps

echo ""
echo -e "${YELLOW}Tailing logs (Ctrl+C to stop viewing, containers keep running)...${NC}"
docker compose -f "$COMPOSE_FILE" --profile "$PROFILE" logs -f
