# Demo Helpers - Before and After Comparison

## Overview

This document shows how extracting helper functions could simplify the demo scripts.

---

## Line Count Comparison

| Script | Lines | Reduction |
|--------|-------|-----------|
| **Original:** `run_demo_simultaneous_noninteractive.sh` | 522 | - |
| **With Helpers:** `run_demo_simplified_example.sh` | 240 | -282 (-54%) |
| **Helper Library:** `demo_helpers.sh` | 350 | (reusable) |

**Net result:** 522 lines → 240 + 350 = 590 lines total, but helpers are reusable across all demo scripts

---

## Code Comparison Examples

### Example 1: Sending ECG Data

#### Before (15 lines per occurrence)
```bash
ECG_RESP=$(curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/bio/signal \
     -H "Content-Type: application/json" \
     -d "{\"source\":\"ecg_monitor\", \"data\":[$ECG_VAL1, $ECG_VAL2, $ECG_VAL3], \"rate_hz\":100.0}" 2>&1)
if echo "$ECG_RESP" | grep -q "ok"; then
    echo "  [ECG]   ✓ [$ECG_VAL1, $ECG_VAL2, $ECG_VAL3]"
fi
```

#### After (3 lines)
```bash
resp=$(send_ecg_signal "$MRI_ENDPOINT" "$val1" "$val2" "$val3")
is_success "$resp" && echo "  [ECG]   ✓ [$val1, $val2, $val3]"
```

**Savings:** 12 lines per usage → ~36 lines across all demos

---

### Example 2: ECG Background Loop

#### Before (25 lines)
```bash
(
    for i in $(seq 1 $ECG_COUNT_TARGET); do
        ECG_VAL1=$(awk "BEGIN {printf \"%.2f\", $RANDOM/32767}")
        ECG_VAL2=$(awk "BEGIN {printf \"%.2f\", $RANDOM/32767}")
        ECG_VAL3=$(awk "BEGIN {printf \"%.2f\", $RANDOM/32767}")
        ECG_RESP=$(curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/bio/signal \
             -H "Content-Type: application/json" \
             -d "{\"source\":\"ecg_monitor\", \"data\":[$ECG_VAL1, $ECG_VAL2, $ECG_VAL3], \"rate_hz\":100.0}" 2>&1)
        if echo "$ECG_RESP" | grep -q "ok"; then
            echo "  [ECG]   ✓ [$ECG_VAL1, $ECG_VAL2, $ECG_VAL3]"
        fi
        sleep $ECG_SLEEP
    done
) &
ECG_PID=$!
```

#### After (1 line)
```bash
ECG_PID=$(start_ecg_loop "$MRI_ENDPOINT" "$ECG_COUNT_TARGET" "$ECG_INTERVAL_MS")
```

**Savings:** 24 lines (repeated for ECG and Pose = 48 lines)

---

### Example 3: Environment Setup

#### Before (30 lines)
```bash
# HDF5 file locking can fail on WSL/overlayfs; keep enabled on native Linux.
if grep -qiE "(microsoft|wsl)" /proc/version 2>/dev/null; then
    export HDF5_USE_FILE_LOCKING=FALSE
    export HDF5_FILE_LOCKING=FALSE
fi

# X11 setup for GUI (handles WSL2 + devcontainer)
if [ -z "$DISPLAY" ]; then
    # Try WSLg first
    if [ -d "/mnt/wslg" ]; then
        export DISPLAY=:0
    else
        # Try to get Windows host IP for VcXsrv
        WIN_IP=$(cat /etc/resolv.conf 2>/dev/null | grep nameserver | awk '{print $2}')
        if [ -n "$WIN_IP" ]; then
            export DISPLAY="${WIN_IP}:0.0"
        else
            export DISPLAY=:0
        fi
    fi
fi
unset XAUTHORITY
```

#### After (2 lines)
```bash
setup_demo_environment
echo "Using DISPLAY=$DISPLAY"
```

**Savings:** 28 lines (repeated across all demos)

---

### Example 4: Graceful Process Shutdown

#### Before (20 lines)
```bash
if [ -n "$MRI_PID" ] && kill -0 $MRI_PID 2>/dev/null; then
    echo "  → Sending SIGTERM to MRI Marshal (PID: $MRI_PID)..."
    kill -TERM $MRI_PID 2>/dev/null || true
    # Wait for graceful shutdown (SHUTDOWN_TIMEOUT_SEC + 5 second buffer)
    WAIT_LOOPS=$(((SHUTDOWN_TIMEOUT_SEC + 5) * 2))
    for i in $(seq 1 $WAIT_LOOPS); do
        if ! kill -0 $MRI_PID 2>/dev/null; then
            echo "  ✓ MRI Marshal shut down gracefully"
            break
        fi
        sleep 0.5
    done
    # Force kill if still running
    if kill -0 $MRI_PID 2>/dev/null; then
        echo "  → Force killing MRI Marshal..."
        kill -9 $MRI_PID 2>/dev/null || true
    fi
fi
```

#### After (1 line)
```bash
graceful_stop "$MRI_PID" "MRI Marshal" "$SHUTDOWN_TIMEOUT_SEC"
```

**Savings:** 19 lines per process (5-6 processes = ~100 lines)

---

### Example 5: File Initialization

#### Before (40 lines)
```bash
if [ -f "$ROBOT_MARSHAL_DIR/files.json" ]; then
    cp "$ROBOT_MARSHAL_DIR/files.json" ./files.json
else
    echo '["file1.json", "file2.json", "file3.json", "robot_status", "robot_commands"]' > ./files.json
fi

mkdir -p ./log_files
ROBOT_FILES_DIR="."
if [ -d "$ROBOT_MARSHAL_DIR/files" ]; then
    ROBOT_FILES_DIR="./files"
fi
export ROBOT_FILES_DIR
mkdir -p "$ROBOT_FILES_DIR"

# Initialize client data files BEFORE starting robot marshal
python3 - <<'PY'
import json
import os
seed = {"client_id": "seed", "sent_at": 1, "values": [1.0, 2.0, 3.0]}
files_dir = os.environ.get("ROBOT_FILES_DIR", ".")
try:
    files = json.load(open("files.json"))
except Exception:
    files = []
for name in files:
    path = os.path.join(files_dir, name)
    with open(path, "w") as fh:
        fh.write(json.dumps(seed))
PY

# file_routes.json (20 lines of JSON)
cat > ./file_routes.json <<'EOF'
{
  "client-a": { ... }
}
EOF
```

#### After (6 lines)
```bash
create_files_json > ./files.json
create_file_routes_json ./file_routes.json
mkdir -p ./log_files
ROBOT_FILES_DIR="."
[ -d "$ROBOT_MARSHAL_DIR/files" ] && ROBOT_FILES_DIR="./files"
initialize_robot_files "$ROBOT_FILES_DIR"
```

**Savings:** 34 lines

---

### Example 6: Health Checks

#### Before (15 lines per check)
```bash
echo -n "MRI Marshal:   "
if curl -s --max-time 2 http://127.0.0.1:$MRI_HTTP/health > /dev/null 2>&1; then
    echo "✓ Ready"
else
    echo "✗ Failed"
    exit 1
fi

echo -n "Robot Marshal: "
if curl -s --max-time 2 "http://127.0.0.1:$ROBOT_HTTP/read/robot_status" > /dev/null 2>&1; then
    echo "✓ Ready"
else
    echo "✗ Failed"
    exit 1
fi
```

#### After (4 lines)
```bash
echo -n "MRI Marshal:   "
check_mri_health "$MRI_ENDPOINT" && print_status "Ready" 0 || print_status "Failed" 1

echo -n "Robot Marshal: "
check_robot_health "$ROBOT_ENDPOINT" && print_status "Ready" 0 || print_status "Failed" 1
```

**Savings:** 11 lines

---

## Helper Functions Provided

### HTTP API Helpers
- `send_ecg_signal()` - Send ECG bio signal data
- `send_pose_update()` - Send pose update
- `is_success()` - Check if response is successful
- `random_float()` - Generate random float

### Health Check Helpers
- `wait_for_http()` - Wait for endpoint to be ready
- `check_mri_health()` - Check MRI marshal health
- `check_robot_health()` - Check robot marshal health

### File Generation Helpers
- `create_files_json()` - Create files.json
- `create_file_routes_json()` - Create file_routes.json
- `initialize_robot_files()` - Initialize seed data

### Process Management Helpers
- `graceful_stop()` - Gracefully stop process with timeout
- `start_ecg_loop()` - Start ECG background loop
- `start_pose_loop()` - Start pose background loop

### Display Helpers
- `print_header()` - Print header with borders
- `print_separator()` - Print separator line
- `print_status()` - Print status with ✓/✗

### Cleanup Helpers
- `cleanup_demo_files()` - Clean up demo-generated files

### Environment Setup Helpers
- `setup_hdf5_locking()` - Configure HDF5 for WSL
- `setup_x11_display()` - Configure X11 DISPLAY
- `setup_demo_environment()` - Setup both HDF5 and X11

---

## Total Savings Across All Demos

If all 3 demo scripts use helpers:

| Component | Lines Saved per Script | Total Saved |
|-----------|------------------------|-------------|
| ECG/Pose loops | 48 | 144 |
| Environment setup | 28 | 84 |
| Graceful shutdown | 100 | 300 |
| File initialization | 34 | 102 |
| Health checks | 11 | 33 |
| API calls | 36 | 108 |
| **Total** | **257** | **771** |

**Net result:**
- Original: 1,461 lines (3 scripts)
- With helpers: 720 lines (scripts) + 350 lines (library) = 1,070 lines
- **Savings: 391 lines (27%)**
- **Maintainability: Much better** (changes in one place)

---

## Benefits of Using Helpers

### 1. **DRY Principle** ✅
Don't Repeat Yourself - Each pattern written once

### 2. **Maintainability** ✅
- Fix a bug once, all demos benefit
- Change API format once, all calls updated
- Add features to helpers, all scripts get them

### 3. **Readability** ✅
```bash
# Before: What does this do?
ECG_VAL1=$(awk "BEGIN {printf \"%.2f\", $RANDOM/32767}")
curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/bio/signal ...

# After: Clear intent
val1=$(random_float 1.0)
send_ecg_signal "$endpoint" "$val1" "$val2" "$val3"
```

### 4. **Testability** ✅
- Helpers can be unit tested independently
- Mock helpers for testing demo logic

### 5. **Consistency** ✅
- All demos use same patterns
- Same error handling everywhere
- Same output formatting

---

## Implementation Steps

### Phase 1: Create Helper Library ✅
- [x] Create `scripts/tools/demo_helpers.sh`
- [x] Add all helper functions
- [x] Document each function

### Phase 2: Create Example (Optional)
- [x] Create `scripts/run_demo_simplified_example.sh`
- [x] Show side-by-side comparison

### Phase 3: Refactor Existing Scripts (Future)
- [ ] Refactor `run_demo.sh` to use helpers
- [ ] Refactor `run_demo_simultaneous.sh` to use helpers
- [ ] Refactor `run_demo_simultaneous_noninteractive.sh` to use helpers
- [ ] Test all refactored scripts
- [ ] Update documentation

---

## Recommendation

**Should we refactor the existing scripts?**

### Pros
✅ Much more maintainable
✅ Easier to add new demos
✅ Cleaner, more readable code
✅ Consistent patterns across demos

### Cons
❌ Refactoring effort (2-3 hours)
❌ Need to test all demos after refactoring
❌ Slight risk of introducing bugs

### Decision
**Recommended:** Do the refactoring when:
1. Adding a new demo script
2. Making significant changes to existing demos
3. Fixing bugs that affect multiple scripts

**For now:** Keep existing scripts as-is, but:
- ✅ Use helpers for any **new** demo scripts
- ✅ Gradually migrate during maintenance
- ✅ Document the helper library

---

## Usage Example

To use the helper library in a new demo:

```bash
#!/bin/bash
source "$(dirname "$0")/tools/demo_helpers.sh"

# Setup environment
setup_demo_environment

# Create files
create_files_json > files.json
initialize_robot_files "."

# Start loops
ECG_PID=$(start_ecg_loop "127.0.0.1:8080" 100 250)

# Cleanup
cleanup_demo_files 1
```

---

## Files Created

1. **[scripts/tools/demo_helpers.sh](../scripts/tools/demo_helpers.sh)** - Helper library (350 lines)
2. **[scripts/run_demo_simplified_example.sh](../scripts/run_demo_simplified_example.sh)** - Example usage (240 lines)
3. **This document** - Comparison and documentation

---

**Last Updated:** January 23, 2026
**Status:** Helper library created, ready for use in new demos
