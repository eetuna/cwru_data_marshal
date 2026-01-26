# FPS Investigation Summary - Quick Reference

**Full details:** [HANDOVER_INVESTIGATION_NEEDED.md](HANDOVER_INVESTIGATION_NEEDED.md)

---

## What Needs Testing

**5 implementations, each for 2 minutes:**

1. **Baseline** - Single-threaded (branch: `main`)
2. **Option 1** - Multi-threaded, no async (branch: `feature/multi-threaded-io`, commit: `4b19d04`)
3. **Option 2** - Multi-threaded + async bio/pose only (branch: `feature/async-write-queue`, commit: `dce99e7`)
4. **Option A** - Multi-threaded + async everything (branch: `feature/async-write-queue`, commit: `72ae6da`)
5. **Option B** - Hybrid: sync MRD + async bio/pose (branch: `feature/async-write-queue`, commit: `6bfab43`)

---

## Why This Investigation

**Previous tests got CONTRADICTORY results:**
- First test: Option B = 37 fps, no drops ✅
- Second test: Option B = 31 fps, 21 zero-FPS drops ❌

**Something is wrong. Need systematic testing to find THE TRUTH.**

---

## Critical Requirements

**Before EVERY test:**
1. ✅ Verify worktree/branch with `git log --oneline -1`
2. ✅ Get user confirmation
3. ✅ Fresh Docker build (verify timestamps)
4. ✅ Run exactly 120 seconds
5. ✅ Save logs with clear names

**NO SHORTCUTS. Follow the procedure exactly.**

---

## Goal (Definition of Done)

**Test Configuration: 128×128×10 (655 KB/frame)**

Find an option that (for 128×128×10):
- ✅ Zero FPS periods: 0
- ✅ Min FPS: ≥ 33 fps
- ✅ Avg FPS: ≥ 35 fps
- ✅ Std Dev: < 8 fps
- ✅ Consistent POST→GET flow

**Phase 2:** Test winner with other configs (64×64×3, 96×96×8) to establish throughput curves.

If no stable option exists → document architectural issues in marshal.

---

## Test Files Expected

```
/tmp/fps_baseline_2min.log   (Baseline)
/tmp/fps_option1_2min.log    (Option 1)
/tmp/fps_option2_2min.log    (Option 2)
/tmp/fps_optionA_2min.log    (Option A)
/tmp/fps_optionB_2min.log    (Option B)

/tmp/build_baseline.log
/tmp/build_option1.log
/tmp/build_option2.log
/tmp/build_optionA.log
/tmp/build_optionB.log

/tmp/fps_final_analysis.txt  (Python analysis)
```

---

## Quick Test Template

```bash
OPTION="option1"
BRANCH="feature/multi-threaded-io"

# 1. Verify worktree
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
git checkout $BRANCH
git log --oneline -1
read -p "Correct branch? [y/N]: " confirm

# 2. Build
cd /workspaces/cwru_data_marshal
sed -i "s|^MRI_BRANCH=.*|MRI_BRANCH=\"$BRANCH\"|" scripts/build-client-images.sh
./scripts/build-client-images.sh > /tmp/build_${OPTION}.log 2>&1

# 3. Verify build
docker images cwru/mri-marshal:latest --format "Built: {{.CreatedAt}}"
read -p "Fresh build? [y/N]: " confirm

# 4. Test
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir
docker stop $(docker ps -q --filter "name=cwru") 2>/dev/null
./scripts/demo-docker.sh > /tmp/demo_${OPTION}.log 2>&1 &
sleep 15
docker compose --env-file .env.demo -f docker-compose.demo.yml --profile viz up -d
sleep 120
docker logs cwru-viz-client 2>&1 | grep "FPS DEBUG" | tee /tmp/fps_${OPTION}_2min.log
docker stop $(docker ps -q --filter "name=cwru")
```

---

## Analysis Command

```bash
python3 /tmp/analyze_all_fps.py > /tmp/fps_final_analysis.txt
cat /tmp/fps_final_analysis.txt
```

(Analysis script is in the full handover document)

---

**CRITICAL:** Provide explicit feedback at EVERY step. Don't assume anything worked.

**TIME ESTIMATE:** ~90 minutes total

**START HERE:** [HANDOVER_INVESTIGATION_NEEDED.md](HANDOVER_INVESTIGATION_NEEDED.md)
