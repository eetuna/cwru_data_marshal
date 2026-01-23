# Demo Scripts - Generated Files and Folders

This document lists all files and folders created when running demo scripts.

---

## Files/Folders Created by Demo Scripts

### Directories Created in Root (`/workspaces/cwru_data_marshal/`)

#### 1. **`data_demo_mri/`**
- **Created by:** All demo scripts (run_demo.sh, run_demo_simultaneous.sh, etc.)
- **Contains:** Subdirectories for each demo run
  - Format: `run_YYYYMMDD_HHMMSS/` (e.g., `run_20260123_155828/`)
  - Each run contains: HDF5 files, server.log, viz.log
- **Size:** Can grow to 100-500MB per run
- **Safe to delete:** ✅ Yes - Regenerated on each demo run

#### 2. **`data_demo_robot/`**
- **Created by:** Demo scripts with robot marshal
- **Contains:** Robot marshal demo outputs
- **Size:** Usually small (< 10KB)
- **Safe to delete:** ✅ Yes - Regenerated on each demo run

#### 3. **`files/`** (conditionally created)
- **Created by:** Demo scripts if `$ROBOT_MARSHAL_DIR/files` exists
- **Contains:** Robot marshal data files (file1.json, file2.json, file3.json, etc.)
- **Location:** Either `./files/` or current directory depending on robot marshal config
- **Safe to delete:** ✅ Yes - Recreated during demo setup

#### 4. **`log_files/`**
- **Created by:** Demo scripts for robot marshal logs
- **Contains:** Robot client log files
- **Size:** Usually small (< 100KB)
- **Safe to delete:** ✅ Yes - Recreated during demo

### Files Created in Root

#### JSON Configuration Files
1. **`files.json`**
   - Lists robot marshal data files
   - Example: `["file1.json", "file2.json", "file3.json", "robot_status", "robot_commands"]`

2. **`file_routes.json`**
   - Defines robot client read/write routes
   - Contains routing configuration for client-a, client-b, client-c

#### Robot Marshal Data Files
Created in root directory (or in `files/` if that directory exists):
- **`file1.json`** - Robot data file 1
- **`file2.json`** - Robot data file 2
- **`file3.json`** - Robot data file 3
- **`robot_status`** - Robot status data
- **`robot_commands`** - Robot commands data

All initialized with seed data: `{"client_id": "seed", "sent_at": 1, "values": [1.0, 2.0, 3.0]}`

#### Log Files
- **`coord.log`** - Old coordinator logs (from previous runs)
- **`mri.log`** - Old MRI logs
- **`robot.log`** - Old robot logs
- **`server.log`** - Old server logs

**Note:** These are leftovers from old runs, not actively written by current demos.

---

## Demo Script Behavior

### During Demo Startup
```bash
# Creates directories
mkdir -p data_demo_mri/run_YYYYMMDD_HHMMSS
mkdir -p data_demo_robot
mkdir -p log_files
mkdir -p files  # If $ROBOT_MARSHAL_DIR/files exists

# Creates configuration files
echo '["file1.json", ...]' > files.json
cat > file_routes.json <<EOF
...
EOF

# Creates robot data files (in current dir or files/)
# file1.json, file2.json, file3.json, robot_status, robot_commands
```

### During Demo Run
- MRI marshal writes to `data_demo_mri/run_*/`
- Robot marshal reads/writes JSON files
- Logs accumulate in `log_files/`

### During Cleanup (if KEEP_DEMO_DATA=0)
```bash
rm -rf data_demo_mri data_demo_robot log_files files
rm -f files.json file_routes.json
rm -f file1.json file2.json file3.json robot_status robot_commands
```

### Current Default (KEEP_DEMO_DATA=1)
```bash
# Keeps: data_demo_mri, data_demo_robot
# Removes: log_files, files, *.json, robot_status, robot_commands
```

---

## Cleanup Commands

### Remove All Demo-Generated Data
```bash
cd /workspaces/cwru_data_marshal

# Directories
rm -rf data_demo_mri
rm -rf data_demo_robot
rm -rf files
rm -rf log_files

# JSON config files
rm -f files.json
rm -f file_routes.json

# Robot data files
rm -f file1.json file2.json file3.json
rm -f robot_status robot_commands

# Old log files
rm -f coord.log mri.log robot.log server.log
```

### Or Use Cleanup Script
```bash
./cleanup_workspace.sh
```
This automatically removes all demo-generated files and directories.

---

## Test Data Directories

Demo scripts may also create test directories (usually from manual testing):
- `data/` - Generic test data
- `data_crash/` - Crash test data
- `data_mri/` - MRI test data
- `data_robot/` - Robot test data
- `test_*/` - Various test output directories

All safe to delete.

---

## Summary Table

| Item | Type | Created By | Size | Safe to Delete |
|------|------|------------|------|----------------|
| `data_demo_mri/` | Directory | All demos | 100-500MB/run | ✅ Yes |
| `data_demo_robot/` | Directory | Demo scripts | < 10KB | ✅ Yes |
| `files/` | Directory | Demo (conditional) | < 1MB | ✅ Yes |
| `log_files/` | Directory | Demo scripts | < 100KB | ✅ Yes |
| `files.json` | File | Demo scripts | < 1KB | ✅ Yes |
| `file_routes.json` | File | Demo scripts | < 1KB | ✅ Yes |
| `file1.json`, `file2.json`, etc. | Files | Demo scripts | < 1KB each | ✅ Yes |
| `robot_status`, `robot_commands` | Files | Demo scripts | < 1KB each | ✅ Yes |
| `*.log` (root) | Files | Old demos | < 1KB each | ✅ Yes |

**Total demo-generated data:** Can accumulate to 1-2GB over multiple runs

---

## Automated Cleanup

The [cleanup_workspace.sh](cleanup_workspace.sh) script handles all demo-generated files automatically.

**What it removes:**
- ✅ All `data_demo_*` directories
- ✅ All `test_*` directories
- ✅ `files/`, `log_files/` directories
- ✅ JSON config files
- ✅ Robot data files
- ✅ Old log files

**What it keeps:**
- ✅ Source code
- ✅ Build directories
- ✅ Documentation
- ✅ Configuration files (Doxyfile, docker-compose.yml, etc.)

---

**Last Updated:** January 23, 2026
