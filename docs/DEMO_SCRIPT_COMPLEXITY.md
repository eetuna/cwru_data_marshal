# Demo Script Complexity Analysis

## Overview

The demo scripts are surprisingly long (400-520 lines each). This document explains why and suggests potential improvements.

---

## Line Counts

| Script | Lines | Purpose |
|--------|-------|---------|
| `run_demo.sh` | 420 | Basic demo with all features |
| `run_demo_simultaneous.sh` | 519 | Interactive simultaneous demo |
| `run_demo_simultaneous_noninteractive.sh` | 522 | Non-interactive simultaneous demo |
| **Total** | **1,461** | |

**Echo statements alone:** 116+ lines of output messages

---

## Why Are They So Long?

### 1. Comprehensive Cleanup Handler (~75 lines)

**Purpose:** Gracefully shut down all processes and clean up files

```bash
cleanup() {
    # Stop producers/clients first
    # Send SIGTERM to marshal with timeout wait
    # Wait up to (SHUTDOWN_TIMEOUT_SEC + 5) seconds
    # Force kill if still running
    # Clean up temp files
    # Remove demo-generated files
}
```

**Breakdown:**
- 5-6 process types to stop (streamer, ECG, pose, clients, marshals)
- Graceful SIGTERM with wait loop (20+ lines)
- Force kill fallback
- Conditional cleanup based on KEEP_DEMO_DATA
- Python script to clean files from files.json
- Multiple pkill commands

**Why it's long:** Needs to handle HDF5 flushing gracefully to prevent data corruption

---

### 2. Environment Setup (~50 lines)

**Purpose:** Configure environment for different platforms (WSL2, native Linux, devcontainer)

```bash
# HDF5 file locking detection
if grep -qiE "(microsoft|wsl)" /proc/version; then
    export HDF5_USE_FILE_LOCKING=FALSE
fi

# X11/DISPLAY configuration
if [ -z "$DISPLAY" ]; then
    if [ -d "/mnt/wslg" ]; then
        export DISPLAY=:0
    else
        WIN_IP=$(cat /etc/resolv.conf | grep nameserver | awk '{print $2}')
        export DISPLAY="${WIN_IP}:0.0"
    fi
fi
```

**Why it's long:** Multiple platform detection paths, fallbacks, and edge cases

---

### 3. Dual Marshal Setup (~100 lines)

**Purpose:** Start both MRI and Robot marshals with health checks

**MRI Marshal Setup (40+ lines):**
- Check if binaries exist
- Rebuild if sources changed
- Start server with configuration
- Health check with curl
- Retry on failure

**Robot Marshal Setup (60+ lines):**
- Copy or create files.json
- Copy or create file_routes.json
- Create log_files directory
- Determine ROBOT_FILES_DIR (. or ./files/)
- Initialize seed data with Python script (15 lines)
- Check if binaries exist
- Build if needed
- Start server
- Health check with retry and rebuild logic (30+ lines)

**Why it's long:** Two separate systems with independent configuration, build checks, and health verification

---

### 4. File Initialization (~60 lines)

**Purpose:** Create configuration and seed data files

```bash
# files.json creation (5 lines or copy)
# file_routes.json creation (20 lines JSON)
# Python seed data script (15 lines)
# Directory creation
```

**Why it's long:** JSON heredocs, Python inline scripts, conditional logic

---

### 5. Simultaneous Operations (~150 lines)

**Purpose:** Run image streamer, ECG, pose, and robot clients concurrently

**Components:**
- Image streamer background process (10 lines)
- ECG sender loop (25 lines)
  - Bash arithmetic for random values
  - curl POST with JSON
  - Error checking
  - Sleep timing
- Pose sender loop (25 lines)
  - Similar to ECG
- Robot clients spawning (30 lines)
  - Parse client list
  - Loop to start each
  - Track PIDs
- Monitor loop (30 lines)
  - Iterate for demo duration
  - Count operations per client
  - Format output
  - Sleep intervals
- Wait and cleanup (20 lines)

**Why it's long:** Bash doesn't have native concurrent primitives, so everything is manual process management

---

### 6. Verbose Output (~100 lines)

**Purpose:** User-friendly progress messages and statistics

**Categories:**
- Header display (10 lines)
- Step headers (20 lines)
- Progress messages during setup (30 lines)
- Live data flow display (20 lines)
- Final statistics (20 lines)

**Example:**
```bash
echo "════════════════════════════════════════════════════════"
echo "          LIVE DATA FLOW (${DEMO_DURATION_SEC} seconds)"
echo "════════════════════════════════════════════════════════"
```

**Why it's long:** Professional presentation with ASCII art borders and detailed status

---

### 7. Error Handling & Retries (~50 lines)

**Purpose:** Handle failures gracefully

**Examples:**
- Health check retries with rebuild
- Binary verification
- Log tail on failure
- Fallback configurations

```bash
if curl -s --max-time 2 "http://..."; then
    echo "✓ Ready"
else
    echo "✗ Failed"
    echo "  → Retrying with rebuild..."
    # 20 lines of retry logic
fi
```

**Why it's long:** Multiple failure modes, each with specific recovery steps

---

### 8. Configuration Parameters (~50 lines)

**Purpose:** Make demos configurable

```bash
# Timing
DEMO_DURATION_SEC=30
IMAGE_INTERVAL_MS=20
ECG_INTERVAL_MS=250
POSE_INTERVAL_MS=500
MONITOR_INTERVAL=0.1
SHUTDOWN_TIMEOUT_SEC=15

# Ports
MRI_HTTP=8080
MRI_WS=8090
ROBOT_HTTP=8081

# Directories
DATA_MRI="./data_demo_mri/run_$(date +%Y%m%d_%H%M%S)"
DATA_ROBOT="./data_demo_robot"

# Feature flags
KEEP_DEMO_DATA=1
```

**Why it's long:** 20+ configurable parameters with comments

---

## Complexity Sources

### 1. **Multi-Process Orchestration**
- Managing 10+ concurrent processes
- Process synchronization
- PID tracking
- Graceful shutdown coordination

### 2. **Platform Compatibility**
- WSL2 vs native Linux
- HDF5 locking differences
- X11/DISPLAY configuration
- File path handling

### 3. **Two Independent Systems**
- MRI marshal (C++ binaries)
- Robot marshal (C++ or Python)
- Different configuration formats
- Separate health checks

### 4. **Production-Quality Polish**
- Graceful shutdown (prevents HDF5 corruption)
- Comprehensive error handling
- Retry logic
- Verbose user feedback
- Statistics collection

### 5. **Bash Limitations**
- No native JSON handling (heredocs instead)
- No native concurrency (background processes instead)
- Manual arithmetic for timing
- Verbose syntax

---

## Could It Be Simplified?

### Possible Improvements

#### 1. **Extract Helper Functions** (Moderate Effort)
```bash
# Before (inline):
curl -s -X POST http://127.0.0.1:$MRI_HTTP/v1/bio/signal \
     -H "Content-Type: application/json" \
     -d "{\"source\":\"ecg_monitor\", \"data\":[$ECG_VAL1, $ECG_VAL2, $ECG_VAL3], \"rate_hz\":100.0}" 2>&1

# After:
send_ecg_data "$ECG_VAL1" "$ECG_VAL2" "$ECG_VAL3"
```

**Savings:** ~50 lines by extracting repeated curl patterns

#### 2. **Python Orchestration** (High Effort)
Rewrite main demo as Python script:
- Native JSON handling
- Better concurrency (asyncio)
- Cleaner error handling
- ~300 lines instead of 520

**Trade-off:** Requires Python dependencies, loses bash simplicity

#### 3. **Configuration File** (Low Effort)
Move parameters to `demo_config.sh`:
```bash
source ./demo_config.sh
```

**Savings:** ~30 lines, better organization

#### 4. **Separate Cleanup Script** (Low Effort)
```bash
source ./scripts/tools/cleanup_functions.sh
```

**Savings:** ~50 lines, reusable

#### 5. **Use `jq` for JSON** (Low Effort)
```bash
# Before: 20-line heredoc
cat > file_routes.json <<EOF
...
EOF

# After:
jq -n '{...}' > file_routes.json
```

**Savings:** ~40 lines across all JSON creation

**Trade-off:** Adds `jq` dependency

---

## Is It Worth Simplifying?

### Pros of Current Approach
✅ **Self-contained** - No external dependencies beyond standard tools
✅ **Well-documented** - Comments explain each section
✅ **Robust** - Handles edge cases and failures
✅ **Educational** - Shows how everything works
✅ **Production-ready** - Graceful shutdown prevents data corruption

### Cons
❌ **Long** - Hard to understand at a glance
❌ **Repetitive** - Similar patterns for ECG/Pose/etc
❌ **Maintenance** - Changes need to be made in multiple places

---

## Recommendations

### Short Term (Low Effort)
1. ✅ **Add comments** - Already done well
2. **Extract configuration** - Move to `demo_config.sh`
3. **Extract cleanup** - Move to `scripts/tools/cleanup_functions.sh`

### Medium Term (Moderate Effort)
1. **Create helper library** - `scripts/tools/demo_helpers.sh`
   - `send_ecg()`, `send_pose()`, `wait_for_health()`, etc.
2. **Reduce verbosity** - Make output toggleable with `--verbose` flag

### Long Term (High Effort)
1. **Python orchestration** - For complex demos
2. **Configuration DSL** - YAML/JSON-based demo definitions

---

## Conclusion

The demo scripts are long primarily because they:
1. **Do a lot** - Orchestrate 10+ processes across 2 systems
2. **Handle errors well** - Graceful shutdown prevents corruption
3. **Support multiple platforms** - WSL2, Linux, devcontainer
4. **Provide excellent UX** - Verbose output, progress, statistics
5. **Use bash** - Verbose language for complex orchestration

**Current state:** Functional but could be more maintainable
**Recommendation:** Small refactorings (extract config, helpers) rather than full rewrite
**Justification:** Scripts work well and are used infrequently (demos, not production)

---

## Line Budget Breakdown

| Category | Lines | % |
|----------|-------|---|
| Configuration | 50 | 10% |
| Environment Setup | 50 | 10% |
| Cleanup Handler | 75 | 14% |
| Marshal Setup | 100 | 19% |
| File Initialization | 60 | 12% |
| Operations Logic | 150 | 29% |
| Output/Display | 100 | 19% |
| Error Handling | 50 | 10% |
| **Total** | **~520** | **100%** |

**Most complex:** Operations logic (29%) and Marshal setup (19%)
**Most reducible:** Output display (19%) and File initialization (12%)

---

**Last Updated:** January 23, 2026
