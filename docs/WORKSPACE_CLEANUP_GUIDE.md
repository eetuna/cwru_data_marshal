# CWRU Data Marshal - Workspace Cleanup Guide

## Overview

This guide explains the workspace structure and what directories/files are safe to delete.

**Total Workspace Size:** ~2.5GB
**Potential Space Savings:** ~1.3GB

---

## Main Repository: `/workspaces/cwru_data_marshal/` (2.2GB)

### ❌ Safe to Delete (Temporary/Generated)

All of these directories are in `.gitignore` and can be regenerated:

#### Large Directories (>1GB)

| Directory | Size | Purpose | Regenerate Method |
|-----------|------|---------|-------------------|
| `data_demo_mri/` | **1.2GB** | Demo run outputs | Run any demo script |
| `build/` | 277MB | CMake build directory | Run `cmake -B build && cmake --build build` |
| `robot_marshal_build_tmp/` | 1.4MB | Old temporary build files | Not needed anymore |

#### Test Data Directories

| Directory | Size | Purpose |
|-----------|------|---------|
| `test_cache_manual/` | 8KB | Test output |
| `test_demo_data/` | 192KB | Test output |
| `test_mrd_data/` | 16KB | Test output |
| `test_ram_cache_data/` | 16KB | Test output |
| `test_shutdown_data/` | 20KB | Test output |
| `test_sigterm_data/` | 16KB | Test output |
| `test_wal_quick/` | 132KB | Test output |

#### Other Temporary Directories

| Directory | Size | Purpose |
|-----------|------|---------|
| `data/` | 12KB | Test/demo data |
| `data_crash/` | 12KB | Crash test data |
| `data_mri/` | 8KB | MRI test data |
| `data_robot/` | 4KB | Robot test data |
| `data_demo_robot/` | 8KB | Demo output |
| `log_files/` | 4KB | Log files |

#### Demo-Generated Files

- `robot_status` - Demo generated
- `robot_commands` - Demo generated
- `files.json` - Demo config
- `file_routes.json` - Demo config

### ⚠️ Check Before Deleting

| Item | Size | Notes |
|------|------|-------|
| `benchmark_data/` | 48KB | Check if benchmark results are valuable |

### ✅ Keep (Essential)

- `.devcontainer/` - Development container config
- `docs/` - Documentation
- `scripts/` - Build and demo scripts
- `archive/` - Archived code/configs
- All source code files

---

## MRI Worktree: `/workspaces/mri_data_marshal_worktree/` (273MB)

**Branch:** `mri-data-marhsal`
**Status:** ✅ **REQUIRED** - Used by demo scripts

### Directory Structure

| Directory | Size | Keep? | Notes |
|-----------|------|-------|-------|
| `build/` | 265MB | ✅ | Contains compiled binaries (marshal, viz_client, image_streamer, etc.) |

This worktree is clean - no temporary files to remove.

---

## Robot Worktree: `/workspaces/robot_data_marshal_catheter_worktree/` (42MB)

**Branch:** `robot_data_marshal_with_catheter_system_components`
**Status:** ✅ **REQUIRED** - Used by demo scripts

### ❌ Safe to Delete

| File/Directory | Size | Purpose |
|----------------|------|---------|
| `log.txt` | 4.2MB | Demo logs |
| `log2.txt` | 1.4MB | Demo logs |
| `log23.txt` | 15MB | Demo logs |
| `log3.txt` | 3.7MB | Demo logs |
| `log4.txt` | 266B | Demo logs |
| `log_files/` | - | Demo logs directory |
| `files/` | - | Demo generated files |
| `files.json` | - | Demo config |
| `file_biological_signals.json` | - | Demo data |

**Space Savings:** ~24MB

### ✅ Keep

| Directory | Size | Notes |
|-----------|------|-------|
| `build/` | 17MB | Contains compiled robot marshal binaries |

---

## Automated Cleanup

A cleanup script has been provided: [`cleanup_workspace.sh`](../cleanup_workspace.sh)

### Usage

```bash
cd /workspaces/cwru_data_marshal
./cleanup_workspace.sh
```

### What the Script Does

1. **Removes all temporary/demo data** from main repository (~1.2GB)
2. **Removes test directories** (~440KB)
3. **Removes log files** from robot worktree (~24MB)
4. **Keeps all source code and compiled binaries**

### Manual Cleanup (Alternative)

If you prefer manual cleanup:

```bash
# Main repository cleanup
cd /workspaces/cwru_data_marshal
rm -rf data_demo_mri data_demo_robot
rm -rf data data_crash data_mri data_robot
rm -rf test_* robot_marshal_build_tmp log_files
rm -f robot_status robot_commands files.json file_routes.json

# Robot worktree cleanup
cd /workspaces/robot_data_marshal_catheter_worktree
rm -f log*.txt
rm -rf log_files files
rm -f files.json file_biological_signals.json
```

---

## Understanding Worktrees

### What Are Worktrees?

Git worktrees allow you to have multiple branches checked out simultaneously in different directories. This project uses worktrees to separate:

- **Main repo** (`main` branch) - Integration and demo scripts
- **MRI worktree** (`mri-data-marhsal` branch) - MRI marshal implementation
- **Robot worktree** (`robot_data_marshal_with_catheter_system_components` branch) - Robot marshal implementation

### Checking Worktrees

```bash
# List all worktrees
git worktree list

# Should show:
# /workspaces/cwru_data_marshal          [main]
# /workspaces/mri_data_marshal_worktree  [mri-data-marhsal]
# /workspaces/robot_data_marshal_catheter_worktree [robot_data_marshal_with_catheter_system_components]
```

### Cleaning Up Broken Worktrees

If worktrees become corrupted (directories deleted but still registered):

```bash
# Show which worktrees will be pruned
git worktree prune --dry-run -v

# Actually prune them
git worktree prune -v
```

The demo scripts will automatically recreate worktrees when needed.

---

## Space Management Tips

### Check Disk Usage

```bash
# Check total workspace size
du -sh /workspaces/*/

# Check specific directories
du -sh /workspaces/cwru_data_marshal/data_demo_mri/*
```

### Keep Only Recent Demo Runs

If you want to keep some demo data but save space:

```bash
cd /workspaces/cwru_data_marshal/data_demo_mri
# Keep only the 2 most recent runs
ls -t | tail -n +3 | xargs rm -rf
```

### Rebuild Binaries if Needed

If you deleted build directories and need to rebuild:

```bash
# Main repo (if it has build targets)
cd /workspaces/cwru_data_marshal
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# MRI worktree (automatically rebuilt by scripts)
# Just run any demo - the scripts will rebuild if needed

# Robot worktree (automatically rebuilt by scripts)
# Just run any demo - the scripts will rebuild if needed
```

---

## Summary

- **Worktrees are essential** - Don't delete the worktree directories themselves
- **Build directories contain binaries** - Keep unless you need to force a rebuild
- **Data/test directories are safe to delete** - All regenerated by running tests/demos
- **Use the provided cleanup script** for safe, automated cleanup
- **Potential savings: ~1.3GB** without affecting functionality
