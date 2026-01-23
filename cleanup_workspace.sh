#!/bin/bash
# Workspace Cleanup Script for CWRU Data Marshal
# Removes temporary files, demo outputs, and test data
# Keeps: source code, compiled binaries, worktrees, and documentation

set -e

MAIN_REPO="/workspaces/cwru_data_marshal"
MRI_WORKTREE="/workspaces/mri_data_marshal_worktree"
ROBOT_WORKTREE="/workspaces/robot_data_marshal_catheter_worktree"

echo "================================================================"
echo "   CWRU DATA MARSHAL - WORKSPACE CLEANUP"
echo "================================================================"
echo ""
echo "This script will remove temporary and generated files to free up space."
echo "Estimated space savings: ~1.3GB"
echo ""
echo "What will be deleted:"
echo "  • Demo output data (data_demo_mri/, data_demo_robot/)"
echo "  • Test output directories (test_*/)"
echo "  • Temporary build files (robot_marshal_build_tmp/)"
echo "  • Log files in worktrees"
echo "  • Demo-generated JSON files"
echo ""
echo "What will be KEPT:"
echo "  ✓ Source code"
echo "  ✓ Build directories with compiled binaries"
echo "  ✓ Worktrees (mri_data_marshal_worktree, robot_data_marshal_catheter_worktree)"
echo "  ✓ Documentation"
echo ""
read -p "Continue with cleanup? (y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Cleanup cancelled."
    exit 0
fi

echo ""
echo "Starting cleanup..."
echo ""

# Function to safely remove directory
safe_remove() {
    local target="$1"
    if [ -d "$target" ] || [ -f "$target" ]; then
        echo "  Removing: $target"
        rm -rf "$target"
    fi
}

# Main repository cleanup
echo "[1/3] Cleaning main repository: $MAIN_REPO"
cd "$MAIN_REPO"

# Demo data directories (1.2GB)
safe_remove "data_demo_mri"
safe_remove "data_demo_robot"

# Test/temporary data directories
safe_remove "data"
safe_remove "data_crash"
safe_remove "data_mri"
safe_remove "data_robot"

# Test directories
safe_remove "test_cache_manual"
safe_remove "test_demo_data"
safe_remove "test_mrd_data"
safe_remove "test_ram_cache_data"
safe_remove "test_shutdown_data"
safe_remove "test_sigterm_data"
safe_remove "test_wal_quick"

# Temporary build directory
safe_remove "robot_marshal_build_tmp"

# Log files
safe_remove "log_files"

# Demo-generated directories
safe_remove "files"

# Demo-generated files
safe_remove "robot_status"
safe_remove "robot_commands"
safe_remove "files.json"
safe_remove "file_routes.json"
safe_remove "file1.json"
safe_remove "file2.json"
safe_remove "file3.json"

# Log files
safe_remove "coord.log"
safe_remove "mri.log"
safe_remove "robot.log"
safe_remove "server.log"

echo ""
echo "[2/3] Cleaning MRI worktree: $MRI_WORKTREE"
# MRI worktree is clean, nothing to remove

echo ""
echo "[3/3] Cleaning Robot worktree: $ROBOT_WORKTREE"
if [ -d "$ROBOT_WORKTREE" ]; then
    cd "$ROBOT_WORKTREE"
    safe_remove "log.txt"
    safe_remove "log2.txt"
    safe_remove "log23.txt"
    safe_remove "log3.txt"
    safe_remove "log4.txt"
    safe_remove "log_files"
    safe_remove "files"
    safe_remove "files.json"
    safe_remove "file_biological_signals.json"
fi

echo ""
echo "================================================================"
echo "   CLEANUP COMPLETE!"
echo "================================================================"
echo ""
echo "Summary of remaining directories:"
du -sh "$MAIN_REPO" "$MRI_WORKTREE" "$ROBOT_WORKTREE" 2>/dev/null || true
echo ""
echo "All temporary files have been removed."
echo "Run demos again to regenerate data as needed."
echo ""
