# Workspace Cleanup Checklist

**Generated:** January 23, 2026
**Purpose:** Complete inventory of temporary/generated files

---

## Quick Cleanup

**Option 1 - Automated:**
```bash
./cleanup_workspace.sh
```

**Option 2 - Manual:** See sections below

---

## Root Directory Files

### ❌ Safe to Delete (Generated/Temporary)

#### Log Files (from old demo runs)
```bash
rm -f coord.log mri.log robot.log server.log
```
- `coord.log` (363 bytes) - Old coordinator log
- `mri.log` (123 bytes) - Old MRI log
- `robot.log` (500 bytes) - Old robot log
- `server.log` (0 bytes) - Empty server log

**Date:** December 21 (over a month old)

### ✅ Keep (Configuration/Documentation)

- `README.md` - Main project readme
- `Doxyfile` - Doxygen configuration
- `docker-compose.yml` - Docker configuration
- `cleanup_workspace.sh` - Cleanup utility
- `ORGANIZATION_SUMMARY.md` - This organization summary

---

## Data Directories

### ❌ Safe to Delete (~1.2GB)

#### Demo Output Directories
```bash
cd /workspaces/cwru_data_marshal
rm -rf data_demo_mri/
rm -rf data_demo_robot/
```

**Size Breakdown:**
- `data_demo_mri/` - 1.2GB
  - `run_20260119_065438` (112M)
  - `run_20260119_065455` (477M)
  - `run_20260119_065541` (235M)
  - `run_20260119_065615` (113M)
  - `run_20260123_155458` (4K)
  - `run_20260123_155551` (4K)
  - `run_20260123_155828` (235M)
  - `run_20260123_160225` (12K)
- `data_demo_robot/` - 8KB

#### Test Output Directories
```bash
rm -rf data/ data_crash/ data_mri/ data_robot/
rm -rf test_*/
```

**Size:** ~500KB total
- `data/` (12KB)
- `data_crash/` (12KB)
- `data_mri/` (8KB)
- `data_robot/` (4KB)
- `test_cache_manual/` (8KB)
- `test_demo_data/` (192KB)
- `test_mrd_data/` (16KB)
- `test_ram_cache_data/` (16KB)
- `test_shutdown_data/` (20KB)
- `test_sigterm_data/` (16KB)
- `test_wal_quick/` (132KB)

#### Temporary Build Files
```bash
rm -rf robot_marshal_build_tmp/
rm -rf log_files/
```

**Size:** ~1.4MB
- `robot_marshal_build_tmp/` (1.4MB) - Old temp files from Jan 5
- `log_files/` (4KB)

---

## Worktree Directories

### MRI Worktree: `/workspaces/mri_data_marshal_worktree/`

**Status:** ✅ Clean - No temporary files

**Keep:**
- `build/` (265MB) - Compiled binaries

### Robot Worktree: `/workspaces/robot_data_marshal_catheter_worktree/`

#### ❌ Delete (~24MB)

```bash
cd /workspaces/robot_data_marshal_catheter_worktree
rm -f log.txt log2.txt log23.txt log3.txt log4.txt
rm -rf log_files/ files/
rm -f files.json file_biological_signals.json
```

**Files:**
- `log.txt` (4.2MB)
- `log2.txt` (1.4MB)
- `log23.txt` (15MB)
- `log3.txt` (3.7MB)
- `log4.txt` (266B)
- `log_files/` (directory)
- `files/` (directory)
- `files.json`
- `file_biological_signals.json`

**Keep:**
- `build/` (17MB) - Compiled binaries

---

## Git Status Files

### ❌ Demo-Generated (from git status)

```bash
rm -f robot_status robot_commands
```

These files are generated during demo runs and recreated as needed.

---

## Summary

### Total Space to Reclaim

| Category | Size | Files/Dirs |
|----------|------|------------|
| Demo output data | ~1.2GB | 2 directories |
| Test directories | ~500KB | 11 directories |
| Temp build files | ~1.4MB | 2 directories |
| Root log files | ~1KB | 4 files |
| Worktree logs | ~24MB | 9 files/dirs |
| **TOTAL** | **~1.225GB** | **28 items** |

### What Stays

| Category | Size | Purpose |
|----------|------|---------|
| Build directories | ~559MB | Compiled binaries |
| Source code | ~10MB | Project code |
| Documentation | ~500KB | Guides and references |
| Configuration | ~50KB | Build configs |
| **TOTAL** | **~570MB** | Essential files |

---

## Automated Cleanup Script

The `cleanup_workspace.sh` script removes all items marked with ❌ above.

### What It Does

1. ✅ Removes demo output directories
2. ✅ Removes test directories
3. ✅ Removes temporary files
4. ✅ Removes log files
5. ✅ Cleans worktree temporary files
6. ❌ Keeps all source code
7. ❌ Keeps build directories (binaries)
8. ❌ Keeps documentation

### Usage

```bash
cd /workspaces/cwru_data_marshal
./cleanup_workspace.sh
```

The script will:
- Show what will be deleted
- Ask for confirmation
- Perform cleanup
- Show final workspace size

---

## Verification

After cleanup, verify with:

```bash
# Check total workspace size
du -sh /workspaces/*/

# Should show approximately:
# 900M    /workspaces/cwru_data_marshal
# 273M    /workspaces/mri_data_marshal_worktree
# 18M     /workspaces/robot_data_marshal_catheter_worktree
```

---

## Regenerating Data

All deleted files can be regenerated:

### Demo Data
```bash
./scripts/run_demo_simultaneous_noninteractive.sh
```

### Test Data
Run individual test scripts in `scripts/`

### Build Outputs
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

**Last Updated:** January 23, 2026
**Status:** Complete inventory of all temporary/generated files
