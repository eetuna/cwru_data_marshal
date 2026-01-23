#!/bin/bash
# Demo helper functions to simplify demo scripts
# Source this file: source scripts/tools/demo_helpers.sh

# ============================================================================
# HTTP API Helpers
# ============================================================================

# Send ECG bio signal data
# Usage: send_ecg_signal <host:port> <val1> <val2> <val3> [rate_hz]
send_ecg_signal() {
    local endpoint="$1"
    local val1="$2"
    local val2="$3"
    local val3="$4"
    local rate_hz="${5:-100.0}"

    curl -s -X POST "http://${endpoint}/v1/bio/signal" \
         -H "Content-Type: application/json" \
         -d "{\"source\":\"ecg_monitor\", \"data\":[$val1, $val2, $val3], \"rate_hz\":$rate_hz}" 2>&1
}

# Send pose update
# Usage: send_pose_update <host:port> <x> <y> <z>
send_pose_update() {
    local endpoint="$1"
    local x="$2"
    local y="$3"
    local z="$4"

    curl -s -X POST "http://${endpoint}/v1/pose/update" \
         -H "Content-Type: application/json" \
         -d "{\"p\": [$x, $y, $z], \"R\": [1,0,0, 0,1,0, 0,0,1]}" 2>&1
}

# Check if response is successful
# Usage: is_success "$response"
is_success() {
    echo "$1" | grep -q "ok"
}

# Generate random float between 0 and max
# Usage: random_float [max]
random_float() {
    local max="${1:-1.0}"
    awk "BEGIN {printf \"%.2f\", ($RANDOM/32767)*$max}"
}

# ============================================================================
# Health Check Helpers
# ============================================================================

# Wait for HTTP endpoint to be ready
# Usage: wait_for_http <url> [max_attempts] [sleep_seconds]
wait_for_http() {
    local url="$1"
    local max_attempts="${2:-10}"
    local sleep_sec="${3:-1}"

    for i in $(seq 1 $max_attempts); do
        if curl -s --max-time 2 "$url" > /dev/null 2>&1; then
            return 0
        fi
        sleep "$sleep_sec"
    done
    return 1
}

# Check MRI marshal health
# Usage: check_mri_health <host:port>
check_mri_health() {
    local endpoint="$1"
    curl -s --max-time 2 "http://${endpoint}/health" > /dev/null 2>&1
}

# Check robot marshal health
# Usage: check_robot_health <host:port> <test_file>
check_robot_health() {
    local endpoint="$1"
    local test_file="${2:-robot_status}"
    curl -s --max-time 2 "http://${endpoint}/read/${test_file}" > /dev/null 2>&1
}

# ============================================================================
# File Generation Helpers
# ============================================================================

# Create files.json for robot marshal
# Usage: create_files_json [file1] [file2] [file3] ...
create_files_json() {
    local files=("$@")
    if [ ${#files[@]} -eq 0 ]; then
        files=("file1.json" "file2.json" "file3.json" "robot_status" "robot_commands")
    fi

    printf '['
    printf '"%s"' "${files[0]}"
    for file in "${files[@]:1}"; do
        printf ',"%s"' "$file"
    done
    printf ']\n'
}

# Create file_routes.json for robot clients
# Usage: create_file_routes_json [output_file]
create_file_routes_json() {
    local output="${1:-./file_routes.json}"
    cat > "$output" <<'EOF'
{
  "client-a": {
    "read_from": "file1.json",
    "write_to1": "file2.json",
    "write_to2": "file3.json"
  },
  "client-b": {
    "read_from": "file2.json",
    "write_to": "file3.json"
  },
  "client-c": {
    "read_from": "file3.json",
    "write_to": "file1.json"
  }
}
EOF
}

# Initialize robot marshal seed data files
# Usage: initialize_robot_files <files_dir> [files_json_path]
initialize_robot_files() {
    local files_dir="${1:-.}"
    local files_json="${2:-./files.json}"

    python3 - <<PYEOF
import json
import os

seed = {"client_id": "seed", "sent_at": 1, "values": [1.0, 2.0, 3.0]}
files_dir = "$files_dir"

try:
    with open("$files_json") as f:
        files = json.load(f)
except Exception:
    files = []

for name in files:
    path = os.path.join(files_dir, name)
    with open(path, "w") as fh:
        fh.write(json.dumps(seed))
PYEOF
}

# ============================================================================
# Process Management Helpers
# ============================================================================

# Gracefully stop a process with timeout
# Usage: graceful_stop <pid> <name> [timeout_sec]
graceful_stop() {
    local pid="$1"
    local name="$2"
    local timeout="${3:-15}"

    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        return 0
    fi

    echo "  → Stopping $name (PID: $pid)..."
    kill -TERM "$pid" 2>/dev/null || return 0

    # Wait for graceful shutdown
    local wait_loops=$((timeout * 2))
    for i in $(seq 1 $wait_loops); do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "  ✓ $name stopped gracefully"
            return 0
        fi
        sleep 0.5
    done

    # Force kill if still running
    if kill -0 "$pid" 2>/dev/null; then
        echo "  → Force killing $name..."
        kill -9 "$pid" 2>/dev/null || true
    fi
}

# Start background loop sending ECG data
# Usage: start_ecg_loop <endpoint> <count> <interval_ms>
# Returns: PID via echo
start_ecg_loop() {
    local endpoint="$1"
    local count="$2"
    local interval_ms="$3"
    local sleep_sec=$(awk "BEGIN {printf \"%.3f\", $interval_ms/1000}")

    (
        for i in $(seq 1 $count); do
            local val1=$(random_float 1.0)
            local val2=$(random_float 1.0)
            local val3=$(random_float 1.0)
            local resp=$(send_ecg_signal "$endpoint" "$val1" "$val2" "$val3")
            if is_success "$resp"; then
                echo "  [ECG]   ✓ [$val1, $val2, $val3]"
            fi
            sleep $sleep_sec
        done
    ) &
    echo $!
}

# Start background loop sending pose data
# Usage: start_pose_loop <endpoint> <count> <interval_ms>
# Returns: PID via echo
start_pose_loop() {
    local endpoint="$1"
    local count="$2"
    local interval_ms="$3"
    local sleep_sec=$(awk "BEGIN {printf \"%.3f\", $interval_ms/1000}")

    (
        for i in $(seq 1 $count); do
            local x=$(random_float 20.0)
            local y=$(random_float 20.0)
            local z=$(random_float 10.0)
            local resp=$(send_pose_update "$endpoint" "$x" "$y" "-$z")
            if is_success "$resp"; then
                echo "  [POSE]  ✓ [$x, $y, -$z]"
            fi
            sleep $sleep_sec
        done
    ) &
    echo $!
}

# ============================================================================
# Display Helpers
# ============================================================================

# Print a header with borders
# Usage: print_header "Title"
print_header() {
    local title="$1"
    local width=65

    echo ""
    printf '=%.0s' $(seq 1 $width)
    echo ""
    printf "%-${width}s\n" "   $title"
    printf '=%.0s' $(seq 1 $width)
    echo ""
}

# Print a section separator
# Usage: print_separator
print_separator() {
    printf '─%.0s' $(seq 1 65)
    echo ""
}

# Print status with checkmark or cross
# Usage: print_status "Message" <0|1>
print_status() {
    local message="$1"
    local success="$2"

    if [ "$success" -eq 0 ]; then
        echo "  ✓ $message"
    else
        echo "  ✗ $message"
    fi
}

# ============================================================================
# Cleanup Helpers
# ============================================================================

# Clean up demo-generated files
# Usage: cleanup_demo_files [keep_data]
cleanup_demo_files() {
    local keep_data="${1:-0}"

    if [ "$keep_data" -eq 0 ]; then
        rm -rf ./data_demo_mri ./data_demo_robot 2>/dev/null || true
    fi

    rm -rf ./log_files ./files 2>/dev/null || true
    rm -f ./files.json ./file_routes.json 2>/dev/null || true

    # Remove files listed in files.json
    if [ -f "./files.json" ]; then
        python3 - <<'PYEOF'
import json, os
try:
    files = json.load(open("files.json"))
except Exception:
    files = []
for name in files:
    try:
        os.remove(name)
    except OSError:
        pass
PYEOF
    fi
}

# ============================================================================
# Environment Setup Helpers
# ============================================================================

# Configure HDF5 file locking for WSL
# Usage: setup_hdf5_locking
setup_hdf5_locking() {
    if grep -qiE "(microsoft|wsl)" /proc/version 2>/dev/null; then
        export HDF5_USE_FILE_LOCKING=FALSE
        export HDF5_FILE_LOCKING=FALSE
        return 0
    fi
    return 1
}

# Configure X11 DISPLAY for WSL/devcontainer
# Usage: setup_x11_display
setup_x11_display() {
    if [ -n "$DISPLAY" ]; then
        return 0
    fi

    # Try WSLg first
    if [ -d "/mnt/wslg" ]; then
        export DISPLAY=:0
        return 0
    fi

    # Try to get Windows host IP for VcXsrv
    local win_ip=$(cat /etc/resolv.conf 2>/dev/null | grep nameserver | awk '{print $2}')
    if [ -n "$win_ip" ]; then
        export DISPLAY="${win_ip}:0.0"
        return 0
    fi

    export DISPLAY=:0
    return 0
}

# Setup demo environment (HDF5 + X11)
# Usage: setup_demo_environment
setup_demo_environment() {
    setup_hdf5_locking
    setup_x11_display
    unset XAUTHORITY
}
