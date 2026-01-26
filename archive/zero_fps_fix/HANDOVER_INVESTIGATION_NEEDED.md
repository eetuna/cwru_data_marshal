# HANDOVER - FPS Performance Investigation Needed

**Date:** 2026-01-25
**Status:** 🚨 **CRITICAL - CONFLICTING RESULTS - INVESTIGATION REQUIRED**
**Previous Agent:** Attempted FPS comparison but got inconsistent results
**Next Agent:** Must perform systematic investigation with extreme caution

---

## 🚨 PROBLEM STATEMENT

**Multiple FPS tests produced CONTRADICTORY results. Something is fundamentally wrong.**

### Conflicting Data Points

**Test 1 (60 seconds):**
- Option B: 37.2 fps average, MIN 23.84 fps, ZERO drops ✅
- Conclusion: "Option B is 2.6x better than baseline"

**Test 2 (120 seconds):**
- Option B: 31.44 fps average, MIN 0.00 fps, 21 ZERO-FPS periods ❌
- Option 1: 32.67 fps average, MIN 7.97 fps, ZERO drops ✅
- Conclusion: "Option 1 is more stable than Option B"

**These results are INCOMPATIBLE. One or both tests are invalid.**

---

## ROOT CAUSE HYPOTHESIS

**Possible issues:**
1. **Worktree confusion** - Tests may have run wrong branches
2. **Image caching** - Docker images not rebuilt between tests
3. **Environment changes** - System state different between tests
4. **Test methodology** - Inconsistent test procedures
5. **Timing issues** - Short tests vs long tests show different behavior
6. **Branch naming confusion** - Multiple branches with similar names

---

## MISSION FOR NEXT AGENT

**Perform a COMPLETE, SYSTEMATIC FPS investigation of ALL options.**

You must test:
1. **Baseline** - Original single-threaded `main` branch
2. **Option 1** - Multi-threaded I/O (`feature/multi-threaded-io`)
3. **Option 2** - Async queue for bio/pose ONLY (`dce99e7` commit on `feature/async-write-queue`)
4. **Option A** - Async queue WITH MRD frames (`72ae6da` commit on `feature/async-write-queue`)
5. **Option B** - Lock optimization (`6bfab43` commit on `feature/async-write-queue`)

Each test MUST:
- Run for **exactly 120 seconds** (2 minutes)
- Use **fresh Docker builds** verified by timestamps
- Provide **explicit worktree confirmation** before each test
- Save **raw FPS logs** with clear naming
- Calculate **min, max, avg, median, std dev, zero-FPS count**

---

## CRITICAL REQUIREMENTS

### ⚠️ WORKTREE SAFETY PROTOCOL

**BEFORE EVERY BUILD AND TEST, you MUST:**

1. **Verify current worktree:**
   ```bash
   cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
   echo "=== WORKTREE VERIFICATION ==="
   echo "Current directory: $(pwd)"
   echo "Current branch: $(git branch --show-current)"
   echo "Latest commit: $(git log --oneline -1)"
   echo "Commit details:"
   git show --stat HEAD
   echo "==========================="
   ```

2. **Explicitly confirm to user:**
   ```
   📍 WORKTREE CONFIRMATION:
      Branch: feature/XYZ
      Commit: abc1234 - commit message
      Files changed in this commit: [list]

   ✅ Ready to build? (User must acknowledge)
   ```

3. **After build, verify images:**
   ```bash
   echo "=== IMAGE VERIFICATION ==="
   docker images cwru/mri-marshal:latest --format "Built: {{.CreatedAt}}"
   docker images cwru/viz-client:latest --format "Built: {{.CreatedAt}}"
   echo "Current time: $(date)"
   echo "==========================="
   ```

4. **Before test, confirm again:**
   ```
   🧪 TEST CONFIRMATION:
      Testing: Option X (feature/XYZ branch)
      Build timestamp: [timestamp]
      Ready to run 120-second test? (User must acknowledge)
   ```

---

## STEP-BY-STEP TEST PROCEDURE

### Pre-Test Setup

1. **Clean environment:**
   ```bash
   docker stop $(docker ps -q --filter "name=cwru") 2>/dev/null || true
   docker rm $(docker ps -aq --filter "name=cwru") 2>/dev/null || true
   docker system prune -f
   ```

2. **Verify X11 available:**
   ```bash
   export DISPLAY=:0
   export WAYLAND_DISPLAY=wayland-0
   export XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir
   ls -la /tmp/.X11-unix/X0 || echo "❌ X11 not available"
   ls -la /mnt/wslg/ || echo "❌ WSLg not available"
   ```

3. **Check session-data permissions:**
   ```bash
   mkdir -p session-data/mrd
   ls -ld session-data/
   # Should be owned by vscode:vscode
   ```

---

### Test Execution Template

**For each option (Baseline, Option 1, Option A, Option B):**

```bash
#!/bin/bash
set -e

OPTION_NAME="option1"  # Change per test
BRANCH_NAME="feature/multi-threaded-io"  # Change per test
TEST_START=$(date +%s)

echo "=========================================="
echo "  TESTING: $OPTION_NAME"
echo "  Branch: $BRANCH_NAME"
echo "  Start time: $(date)"
echo "=========================================="

# Step 1: Switch to correct branch
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
git checkout "$BRANCH_NAME"
git pull origin "$BRANCH_NAME" || true

# Step 2: VERIFY worktree (MANDATORY)
echo ""
echo "📍 WORKTREE VERIFICATION:"
echo "   PWD: $(pwd)"
echo "   Branch: $(git branch --show-current)"
echo "   Commit: $(git log --oneline -1)"
git show --stat HEAD | head -20
echo ""
read -p "✅ Confirm this is correct branch for $OPTION_NAME [y/N]: " confirm
if [ "$confirm" != "y" ]; then
    echo "❌ Test aborted by user"
    exit 1
fi

# Step 3: Update build script
cd /workspaces/cwru_data_marshal
sed -i "s|^MRI_BRANCH=.*|MRI_BRANCH=\"$BRANCH_NAME\"|" scripts/build-client-images.sh
grep "MRI_BRANCH=" scripts/build-client-images.sh | head -3

# Step 4: Build images
echo ""
echo "🔨 Building Docker images..."
./scripts/build-client-images.sh > /tmp/build_${OPTION_NAME}.log 2>&1
BUILD_EXIT=$?
if [ $BUILD_EXIT -ne 0 ]; then
    echo "❌ Build failed! Check /tmp/build_${OPTION_NAME}.log"
    exit 1
fi

# Step 5: VERIFY build timestamp
echo ""
echo "🔍 IMAGE VERIFICATION:"
docker images cwru/mri-marshal:latest --format "Built: {{.CreatedAt}}"
docker images cwru/viz-client:latest --format "Built: {{.CreatedAt}}"
echo "Current time: $(date)"
echo ""
read -p "✅ Confirm images were just built [y/N]: " confirm
if [ "$confirm" != "y" ]; then
    echo "❌ Test aborted by user"
    exit 1
fi

# Step 6: Clean environment
docker stop $(docker ps -q --filter "name=cwru") 2>/dev/null || true
docker rm $(docker ps -aq --filter "name=cwru") 2>/dev/null || true
rm -rf session-data/run_* 2>/dev/null || true

# Step 7: Start demo
echo ""
echo "🚀 Starting demo..."
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir
./scripts/demo-docker.sh > /tmp/demo_${OPTION_NAME}.log 2>&1 &
DEMO_PID=$!
echo "Demo PID: $DEMO_PID"

# Step 8: Wait for services and start viz
sleep 15
echo "Starting viz client..."
docker compose --env-file .env.demo -f docker-compose.demo.yml --profile viz up -d

# Verify viz started
sleep 5
if ! docker ps | grep -q "cwru-viz-client"; then
    echo "❌ Viz client failed to start"
    docker logs cwru-viz-client 2>&1 | tail -20
    kill $DEMO_PID 2>/dev/null
    exit 1
fi
echo "✅ Viz client running"

# Step 9: Collect FPS for EXACTLY 120 seconds
echo ""
echo "⏱️  Collecting FPS data for 120 seconds..."
echo "Start time: $(date)"
sleep 120
echo "End time: $(date)"

# Step 10: Extract FPS data
echo ""
echo "📊 Extracting FPS measurements..."
docker logs cwru-viz-client 2>&1 | grep "FPS DEBUG" | tee /tmp/fps_${OPTION_NAME}_2min.log

# Step 11: Verify data collected
SAMPLE_COUNT=$(wc -l < /tmp/fps_${OPTION_NAME}_2min.log)
echo ""
echo "Collected $SAMPLE_COUNT FPS samples"
if [ $SAMPLE_COUNT -lt 50 ]; then
    echo "⚠️  WARNING: Low sample count (expected ~120)"
fi

# Step 12: Quick analysis
python3 << 'PYEOF'
import re
with open('/tmp/fps_${OPTION_NAME}_2min.log', 'r') as f:
    fps_values = [float(re.search(r'FPS: ([\d.]+)', line).group(1))
                  for line in f if 'FPS:' in line]
if fps_values:
    print(f"\n📈 Quick Stats:")
    print(f"   Min:  {min(fps_values):.2f} fps")
    print(f"   Max:  {max(fps_values):.2f} fps")
    print(f"   Avg:  {sum(fps_values)/len(fps_values):.2f} fps")
    print(f"   Zeros: {sum(1 for x in fps_values if x == 0)}")
PYEOF

# Step 13: Cleanup
echo ""
echo "🧹 Cleaning up..."
kill $DEMO_PID 2>/dev/null || true
docker stop $(docker ps -q --filter "name=cwru") 2>/dev/null

# Step 14: Archive results
TEST_END=$(date +%s)
TEST_DURATION=$((TEST_END - TEST_START))
echo ""
echo "=========================================="
echo "  TEST COMPLETE: $OPTION_NAME"
echo "  Duration: ${TEST_DURATION}s"
echo "  FPS data: /tmp/fps_${OPTION_NAME}_2min.log"
echo "  Build log: /tmp/build_${OPTION_NAME}.log"
echo "  Demo log: /tmp/demo_${OPTION_NAME}.log"
echo "=========================================="
```

---

## BRANCH DETAILS

### 1. Baseline (Original)

**Branch:** `main` or `mri-data-marhsal`
**Commit:** `3d86997` or later on main
**Description:** Single-threaded implementation, no async queue
**Expected FPS:** ~10-15 fps (from handover docs)
**Files:** `/tmp/fps_baseline_2min.log`

**To test:**
```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
git checkout main  # or mri-data-marhsal branch
git log --oneline -3
```

**Key characteristics:**
- Single-threaded io_context
- All HTTP requests serialized
- Simple, original implementation
- No threading complexity

### 2. Option 1 (Multi-threaded I/O)

**Branch:** `feature/multi-threaded-io`
**Commit:** `4b19d04` (has FPS debug logging)
**Description:** 4 HTTP handler threads, coarse-grained locking, NO async queue
**Expected FPS:** Unknown (conflicting reports: 14 fps vs 32 fps)
**Files:** `/tmp/fps_option1_2min.log`

**To test:**
```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
git checkout feature/multi-threaded-io
git log --oneline -3
```

**Key characteristics:**
- 4 concurrent HTTP handler threads
- Entire write operation under single lock
- Lock hold time: ~50ms per frame
- No async queue for any data type
- MRD frames written synchronously
- Simple, proven architecture

### 3. Option 2 (Async Queue for Bio/Pose Only)

**Branch:** `feature/async-write-queue`
**Commit:** `dce99e7` - "feat: Add async write queue for non-blocking disk I/O"
**Description:** Option 1 + async queue for bio/pose signals ONLY, MRD still synchronous
**Expected FPS:** Unknown (suspected unstable based on handover docs)
**Files:** `/tmp/fps_option2_2min.log`

**To test:**
```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
git checkout feature/async-write-queue
git reset --hard dce99e7
git show --stat  # Verify this is the original async queue commit
```

**Key characteristics:**
- 4 concurrent HTTP handler threads
- Bio signals → async queue → background writer thread
- Pose updates → async queue → background writer thread
- **MRD frames still SYNCHRONOUS** (written in HTTP thread)
- Lock contention on mrd_sink from multiple threads
- This is what caused FPS drops according to handover docs

### 4. Option A (Async Queue WITH MRD Frames)

**Branch:** `feature/async-write-queue`
**Commit:** `72ae6da` - "fix: Implement async MRD frame writes"
**Description:** Background writer thread with async queue for MRD frames
**Expected FPS:** Unknown (handover says 0 fps periods, needs verification)
**Files:** `/tmp/fps_optionA_2min.log`

**To test this specific commit:**
```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
git checkout feature/async-write-queue
git reset --hard 72ae6da
git show --stat  # Verify this is the async queue commit
```

**Key characteristics:**
- MRD frames copied to queue (1 MB per frame)
- Background writer processes queue
- Returns `"path": "[queued]"` placeholder
- Memory overhead: ~10 MB (queue depth)

### 5. Option B (Lock Optimization - Hybrid)

**Branch:** `feature/async-write-queue`
**Commit:** `6bfab43` - "feat: Implement Option B lock scope optimization"
**Description:** Fine-grained locks for MRD + async queue for bio/pose (HYBRID)
**Expected FPS:** Unknown (conflicting: 37 fps with no drops vs 31 fps with 21 drops)
**Files:** `/tmp/fps_optionB_2min.log`

**To test:**
```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
git checkout feature/async-write-queue
git reset --hard 6bfab43
git show --stat  # Verify this is the lock optimization commit
```

**Key characteristics:**
- **HYBRID ARCHITECTURE**
- MRD frames: Synchronous writes with fine-grained locks
  - Lock hold time: ~0.1ms (two brief locks per frame)
  - SWMR concurrent writes for H5Dwrite
  - Most operations outside lock
- Bio/Pose: Async queue → background writer thread
- Reverts Option A's async MRD queue
- Most complex implementation

---

## VERIFICATION CHECKLIST

After ALL tests complete, verify:

```bash
# 1. Check all test files exist
ls -lh /tmp/fps_baseline_2min.log
ls -lh /tmp/fps_option1_2min.log
ls -lh /tmp/fps_option2_2min.log
ls -lh /tmp/fps_optionA_2min.log
ls -lh /tmp/fps_optionB_2min.log

# 2. Check sample counts (should be ~115-125 each)
wc -l /tmp/fps_*_2min.log

# 3. Check build logs for errors
grep -i error /tmp/build_*.log || echo "No build errors"

# 4. Quick stats for all
for f in /tmp/fps_*_2min.log; do
    echo "=== $(basename $f) ==="
    grep "FPS:" $f | awk -F'FPS: ' '{print $2}' | sort -n | awk '
        {sum+=$1; if(NR==1){min=$1} max=$1}
        END {print "Min: "min" Max: "max" Avg: "sum/NR}
    '
done
```

---

## COMPREHENSIVE ANALYSIS SCRIPT

After collecting all data, run this analysis:

```python
#!/usr/bin/env python3
import re
import sys

def analyze_fps_file(filepath, name):
    """Analyze single FPS log file"""
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
    except FileNotFoundError:
        return None

    fps_values = []
    for line in lines:
        match = re.search(r'FPS: ([\d.]+)', line)
        if match:
            fps_values.append(float(match.group(1)))

    if not fps_values:
        return None

    fps_values.sort()
    n = len(fps_values)

    # Calculate statistics
    min_fps = min(fps_values)
    max_fps = max(fps_values)
    avg_fps = sum(fps_values) / n
    median_fps = fps_values[n//2] if n % 2 else (fps_values[n//2-1] + fps_values[n//2]) / 2

    # Standard deviation
    variance = sum((x - avg_fps) ** 2 for x in fps_values) / n
    std_dev = variance ** 0.5

    # Percentiles
    p25 = fps_values[n // 4]
    p75 = fps_values[3 * n // 4]
    p95 = fps_values[int(n * 0.95)]

    # Problem areas
    zero_fps = sum(1 for x in fps_values if x == 0)
    below_5 = sum(1 for x in fps_values if 0 < x < 5)
    below_10 = sum(1 for x in fps_values if 0 < x < 10)

    return {
        'name': name,
        'samples': n,
        'min': min_fps,
        'max': max_fps,
        'avg': avg_fps,
        'median': median_fps,
        'std': std_dev,
        'p25': p25,
        'p75': p75,
        'p95': p95,
        'zeros': zero_fps,
        'below_5': below_5,
        'below_10': below_10,
        'data': fps_values
    }

# Analyze all options
options = [
    ('/tmp/fps_baseline_2min.log', 'Baseline (Single-threaded)'),
    ('/tmp/fps_option1_2min.log', 'Option 1 (Multi-threaded)'),
    ('/tmp/fps_option2_2min.log', 'Option 2 (Async Bio/Pose)'),
    ('/tmp/fps_optionA_2min.log', 'Option A (Async All)'),
    ('/tmp/fps_optionB_2min.log', 'Option B (Hybrid)'),
]

results = []
for filepath, name in options:
    result = analyze_fps_file(filepath, name)
    if result:
        results.append(result)
        print(f"\n{'='*60}")
        print(f"{name}")
        print(f"{'='*60}")
        print(f"Samples:        {result['samples']}")
        print(f"Min FPS:        {result['min']:.2f}")
        print(f"Max FPS:        {result['max']:.2f}")
        print(f"Avg FPS:        {result['avg']:.2f}")
        print(f"Median FPS:     {result['median']:.2f}")
        print(f"Std Dev:        {result['std']:.2f}")
        print(f"25th %ile:      {result['p25']:.2f}")
        print(f"75th %ile:      {result['p75']:.2f}")
        print(f"95th %ile:      {result['p95']:.2f}")
        print(f"")
        print(f"Zero FPS:       {result['zeros']} ({result['zeros']/result['samples']*100:.1f}%)")
        print(f"Below 5 fps:    {result['below_5']} ({result['below_5']/result['samples']*100:.1f}%)")
        print(f"Below 10 fps:   {result['below_10']} ({result['below_10']/result['samples']*100:.1f}%)")
    else:
        print(f"\n⚠️  {name}: No data found at {filepath}")

# Comparison table
if len(results) >= 2:
    print(f"\n{'='*80}")
    print(f"COMPARISON TABLE")
    print(f"{'='*80}")
    print(f"{'Metric':<20} " + " ".join([f"{r['name'][:15]:>15}" for r in results]))
    print(f"{'-'*80}")

    metrics = [
        ('Avg FPS', 'avg'),
        ('Min FPS', 'min'),
        ('Max FPS', 'max'),
        ('Median FPS', 'median'),
        ('Std Dev', 'std'),
        ('Zero FPS', 'zeros'),
        ('Below 10 fps', 'below_10'),
    ]

    for label, key in metrics:
        row = f"{label:<20}"
        for r in results:
            if key in ['zeros', 'below_10']:
                row += f" {r[key]:>15}"
            else:
                row += f" {r[key]:>15.2f}"
        print(row)

# Winner determination
print(f"\n{'='*80}")
print(f"RECOMMENDATIONS")
print(f"{'='*80}")

# Sort by average FPS (descending) but penalize zero FPS
ranked = sorted(results, key=lambda r: r['avg'] - (r['zeros'] * 10), reverse=True)

for i, r in enumerate(ranked, 1):
    emoji = "🥇" if i == 1 else "🥈" if i == 2 else "🥉" if i == 3 else "  "
    stability = "✅ Stable" if r['zeros'] == 0 and r['below_5'] < 2 else "⚠️  Unstable" if r['zeros'] < 5 else "❌ Critical"
    print(f"{emoji} #{i}: {r['name']}")
    print(f"      Avg: {r['avg']:.2f} fps, Zeros: {r['zeros']}, {stability}")

# Final recommendation
best = ranked[0]
if best['zeros'] > 0:
    print(f"\n⚠️  WARNING: Top performer has {best['zeros']} zero-FPS periods")
    print(f"    Consider next most stable option instead")

    # Find most stable
    stable = [r for r in results if r['zeros'] == 0]
    if stable:
        best_stable = max(stable, key=lambda r: r['avg'])
        print(f"\n✅ RECOMMENDED: {best_stable['name']}")
        print(f"   Avg: {best_stable['avg']:.2f} fps, Zero FPS: 0, Stable")
    else:
        print(f"\n❌ NO STABLE OPTIONS FOUND - All have zero-FPS periods")
else:
    print(f"\n✅ RECOMMENDED: {best['name']}")
    print(f"   Avg: {best['avg']:.2f} fps, Zero FPS: 0, Stable")

print(f"\n{'='*80}")
```

Save as `/tmp/analyze_all_fps.py` and run:
```bash
python3 /tmp/analyze_all_fps.py > /tmp/fps_final_analysis.txt
cat /tmp/fps_final_analysis.txt
```

---

## DELIVERABLES

At the end of investigation, provide:

1. **Test logs:**
   - `/tmp/fps_baseline_2min.log`
   - `/tmp/fps_option1_2min.log`
   - `/tmp/fps_option2_2min.log`
   - `/tmp/fps_optionA_2min.log`
   - `/tmp/fps_optionB_2min.log`

2. **Build logs:**
   - `/tmp/build_baseline.log`
   - `/tmp/build_option1.log`
   - `/tmp/build_option2.log`
   - `/tmp/build_optionA.log`
   - `/tmp/build_optionB.log`

3. **Analysis:**
   - `/tmp/fps_final_analysis.txt`

4. **Final report:**
   - `FPS_INVESTIGATION_FINAL_REPORT.md` with:
     - Clear winner identification
     - Statistical comparison table
     - Stability assessment
     - Production recommendation
     - Root cause of previous conflicting results

5. **Verification evidence:**
   - Screenshots or logs showing worktree state before each test
   - Docker image build timestamps
   - Git commit SHAs tested

---

## DEFINITION OF DONE

**Primary Goal:** Resolve FPS dropouts and achieve consistent write (POST) / read (GET) performance across the entire system.

### Success Criteria (Configuration-Dependent)

**Test Configuration: 128×128×10 (655 KB/frame)**

Success criteria depend on image configuration. Larger frames = lower achievable FPS.

| Configuration | Frame Size | Target FPS | Min Acceptable |
|--------------|------------|------------|----------------|
| 64×64×3 | 49 KB | 46-50 fps | 45 fps |
| 96×96×8 | 295 KB | 37-40 fps | 35 fps |
| **128×128×10** | **655 KB** | **35-37 fps** | **33 fps** |

1. ✅ **Identify root cause** of FPS dropouts (0 fps periods, drops below threshold)
2. ✅ **Determine if architectural issue** exists in marshal design
3. ✅ **Find option with stable performance (for 128×128×10):**
   - Zero FPS periods: 0 (no complete stalls)
   - Min FPS: ≥ 33 fps (configuration-dependent minimum)
   - Avg FPS: ≥ 35 fps (good throughput for this frame size)
   - Std Dev: < 8 fps (consistent performance)
4. ✅ **Verify write-read consistency:**
   - POST /v1/mrd/image → immediate flush
   - GET /v1/mrd/latest → returns fresh data
   - No stale data served to viz client
   - SWMR refresh works correctly
5. ✅ **Production-ready recommendation:**
   - Clear winner identified
   - Stability proven over 2-minute test
   - Known limitations documented
   - Merge/deployment instructions provided
6. ✅ **Throughput limit testing (Phase 2):**
   - Test winner with multiple configurations (64×64×3, 96×96×8, 128×128×10)
   - Establish throughput curves
   - Verify performance scales correctly

### If No Stable Option Found

If ALL options show instability:
- Document architectural issues in marshal
- Recommend redesign approach
- Identify bottlenecks (HDF5, SWMR, locks, I/O)
- Provide alternative architecture proposals

---

## CRITICAL SUCCESS FACTORS

1. ✅ **Test ALL FIVE options** (Baseline, Option 1, Option 2, A, B)
2. ✅ **Each test runs exactly 120 seconds**
3. ✅ **Fresh Docker builds verified by timestamp**
4. ✅ **Explicit worktree confirmation** before EVERY test
5. ✅ **User acknowledgment** of branch/commit before each test
6. ✅ **Raw FPS logs saved** with clear naming
7. ✅ **Complete statistical analysis** (min, max, avg, median, std, zeros)
8. ✅ **Clear production recommendation** with rationale
9. ✅ **Write-read consistency verified** (POST→GET flow works)
10. ✅ **Root cause analysis** of any instability found

---

## RED FLAGS TO WATCH FOR

🚩 **Sample count < 100** - Test may have failed, viz client crashed
🚩 **Zero FPS > 10%** - Critical instability, option unsuitable
🚩 **Std Dev > 15** - High variance, unpredictable performance
🚩 **Build timestamp old** - Docker cache hit, not fresh build
🚩 **Worktree mismatch** - Wrong branch tested

---

## WRITE-READ CONSISTENCY TESTING

**After identifying stable option(s), verify POST→GET flow:**

### Test 1: Immediate Flush Verification

```bash
# Start demo with winning option
./scripts/demo-docker.sh &
sleep 10

# Monitor index.jsonl in real-time
docker exec cwru-mri-marshal tail -f /session-data/run_*/mrd/index.jsonl &
INDEX_PID=$!

# Send test frame
curl -X POST http://localhost:8080/v1/mrd/image \
  -H "Content-Type: application/octet-stream" \
  --data-binary @test_frame.bin

# Check if flushed
sleep 0.5
docker exec cwru-mri-marshal tail -1 /session-data/run_*/mrd/index.jsonl | jq '.flushed'
# Should be: true

kill $INDEX_PID
```

### Test 2: GET /latest Returns Fresh Data

```bash
# Get baseline frame index
BEFORE=$(curl -s http://localhost:8080/v1/mrd/latest | jq '.frame_index')

# POST new frame
curl -X POST http://localhost:8080/v1/mrd/image -H "Content-Type: application/octet-stream" --data-binary @test_frame.bin

# GET latest immediately
AFTER=$(curl -s http://localhost:8080/v1/mrd/latest | jq '.frame_index')

echo "Before: $BEFORE, After: $AFTER"
# After should be > Before (or Before+1 if single frame)
```

### Test 3: Viz Client Gets Fresh Frames

```bash
# Start viz client with logging
docker logs -f cwru-viz-client 2>&1 | grep "viz: frame" &
VIZ_PID=$!

# Watch for frame updates
sleep 5
kill $VIZ_PID

# Check that frame indices are increasing
docker logs cwru-viz-client 2>&1 | grep "viz: frame" | tail -20
# Should show: frame 100, frame 101, frame 102, ... (increasing)
```

### Test 4: SWMR Refresh Works

```bash
# Check H5Drefresh is called successfully
docker logs cwru-viz-client 2>&1 | grep -i "refresh\|swmr" || echo "No SWMR logs"

# Verify no stale data warnings
docker logs cwru-viz-client 2>&1 | grep -i "stale\|old\|outdated" || echo "No stale data warnings"
```

---

## DEBUGGING TIPS

If viz client fails to start:
```bash
# Check viz client logs immediately
docker logs cwru-viz-client 2>&1

# Check if waiting for WebSocket
docker exec cwru-viz-client ps aux | grep websocat

# Check if mri-marshal is responding
curl -s http://localhost:8080/v1/mrd/latest | jq .

# Check WebSocket server
echo '{"subscribe":"mrd"}' | websocat ws://localhost:8090/ws
```

If FPS is 0:
```bash
# Check image-streamer is sending frames
docker logs cwru-image-streamer 2>&1 | grep "frame" | tail -10

# Check MRD files are growing
watch -n 1 'ls -lh session-data/run_*/mrd/*.mrd'

# Check viz client is reading
docker exec cwru-viz-client lsof | grep .mrd
```

---

## EXPECTED TIMELINE

- **Setup and verification:** 15 minutes
- **Baseline test:** 10 minutes (build + 2min test + analysis)
- **Option 1 test:** 10 minutes
- **Option 2 test:** 10 minutes
- **Option A test:** 10 minutes
- **Option B test:** 10 minutes
- **Write-read consistency testing:** 15 minutes
- **Final analysis and report:** 20 minutes

**Total:** ~90 minutes for complete investigation

---

## FINAL NOTE

**The previous agent got conflicting results. Your job is to determine THE TRUTH through systematic, careful testing.**

Be methodical. Verify everything. Provide explicit feedback at each step.

Good luck! 🔍

---

**Handover Date:** 2026-01-25
**Priority:** 🔥 HIGH
**Complexity:** 🔴 COMPLEX - Requires careful attention to detail
