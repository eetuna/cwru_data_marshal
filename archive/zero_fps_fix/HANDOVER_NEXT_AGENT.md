# HANDOVER - FPS Investigation Continuation

**Date:** 2026-01-25
**Previous Agent:** Added FPS DEBUG logging to baseline, completed baseline test
**Next Agent:** Continue testing Options 1, 2, A, B

---

## COMPLETED WORK

### 1. Configuration Updated
- `.env.demo` set to 128×128×10 (655 KB/frame)
- `DEMO_DURATION=0` (infinite - manually stop after test)
- Success criteria updated in handover docs

### 2. FPS DEBUG Logging Added to Baseline
```
Branch: mri-data-marhsal
Commit: 48372c9 (just added)
File: clients/viz_client/viz_client_main.cpp
```

### 3. Baseline Test Results (128×128×10)
```
Avg FPS: 20.11
Min FPS: 4.92 (severe drops!)
Max FPS: 30.54
Below 20 FPS: 39.4%
Below 30 FPS: 95.5%

VERDICT: FAILS (target was 33-37 FPS)
```

---

## FPS DEBUG LOGGING STATUS

| Option | Branch/Commit | Has FPS DEBUG | Notes |
|--------|---------------|---------------|-------|
| Baseline | mri-data-marhsal (48372c9) | ✅ YES | Just added |
| Option 1 | feature/multi-threaded-io (4b19d04) | ✅ YES | Already had it |
| Option 2 | feature/async-write-queue (dce99e7) | ❌ NO | NEEDS ADDING |
| Option A | feature/async-write-queue (72ae6da) | ✅ YES | Already has it |
| Option B | feature/async-write-queue (6bfab43) | ✅ YES | Already has it |

### To Add FPS DEBUG to Option 2 (dce99e7):
```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
git checkout feature/async-write-queue
git checkout dce99e7  # Option 2 commit

# Add to clients/viz_client/viz_client_main.cpp after line ~503:
std::cerr << "[FPS DEBUG] Elapsed: " << fps_elapsed << "s, Frames: " << frame_count
          << ", FPS: " << current_fps << "\n" << std::flush;

# Commit or build directly
```

---

## REMAINING TESTS

### Phase 1: Test All Options (128×128×10)

| # | Option | Branch | Commit | Status |
|---|--------|--------|--------|--------|
| 1 | Baseline | mri-data-marhsal | 48372c9 | ✅ DONE - 20 FPS avg, FAILS |
| 2 | Option 1 | feature/multi-threaded-io | 4b19d04 | ⏳ PENDING |
| 3 | Option 2 | feature/async-write-queue | dce99e7 | ⏳ PENDING (add FPS first) |
| 4 | Option A | feature/async-write-queue | 72ae6da | ⏳ PENDING |
| 5 | Option B | feature/async-write-queue | 6bfab43 | ⏳ PENDING |

### Phase 2: Throughput Testing (winner only)
- Test with 64×64×3 (target: 45-50 FPS)
- Test with 96×96×8 (target: 35-40 FPS)

---

## SUCCESS CRITERIA (128×128×10)

- **Zero FPS periods:** 0
- **Min FPS:** ≥ 33 FPS
- **Avg FPS:** ≥ 35 FPS
- **Std Dev:** < 8 FPS

---

## TEST PROCEDURE

### For Each Option:

1. **Switch branch in worktree:**
```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
git checkout <branch>
git reset --hard <commit>  # For specific commits
git log --oneline -1  # Verify
```

2. **Update build script:**
```bash
# Edit scripts/build-client-images.sh line 21:
MRI_BRANCH="<branch-name>"
```

3. **Build images:**
```bash
./scripts/build-client-images.sh > /tmp/build_<option>.log 2>&1
docker images cwru/mri-marshal:latest --format "Built: {{.CreatedAt}}"
```

4. **Run test (120 seconds):**
```bash
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir
./scripts/demo-docker.sh > /tmp/demo_<option>.log 2>&1 &
sleep 130  # 120s test + buffer
```

5. **Extract FPS data:**
```bash
docker logs cwru-viz-client 2>&1 | grep "FPS DEBUG" > /tmp/fps_<option>_128x128x10.log
```

6. **Analyze:**
```python
import re
with open('/tmp/fps_<option>_128x128x10.log') as f:
    fps = [float(re.search(r'FPS: ([\d.]+)', l).group(1)) for l in f if 'FPS:' in l]
print(f"Min: {min(fps):.2f}, Max: {max(fps):.2f}, Avg: {sum(fps)/len(fps):.2f}")
print(f"Zeros: {sum(1 for x in fps if x == 0)}")
```

7. **Stop demo:**
```bash
docker stop $(docker ps -q --filter "name=cwru")
```

---

## CURRENT STATE

- **Worktree:** `/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal`
- **Current branch:** `mri-data-marhsal` (baseline with FPS logging)
- **Build script:** Set to `mri-data-marhsal`
- **All cwru containers:** Stopped

---

## FILES CREATED

- `/tmp/fps_baseline_128x128x10.log` - Baseline results
- `/tmp/build_baseline.log` - Build log
- `HANDOVER_NEXT_AGENT.md` - This file

---

## EXPECTED DELIVERABLES

After completing all tests:
1. `/tmp/fps_option1_128x128x10.log`
2. `/tmp/fps_option2_128x128x10.log`
3. `/tmp/fps_optionA_128x128x10.log`
4. `/tmp/fps_optionB_128x128x10.log`
5. `FPS_INVESTIGATION_FINAL_REPORT.md`

---

## KEY FINDINGS SO FAR

**Baseline (single-threaded) at 128×128×10:**
- Only achieves ~20 FPS average
- Has severe drops to 5 FPS (39% of samples below 20 FPS)
- Does NOT meet the 33-37 FPS target
- Multi-threading is clearly needed for this workload

---

**Good luck! The baseline has failed - now test if the multi-threaded options can meet the target.**
