# Handoff — full-stack comparison + stress test (2026-04-19)

**For:** next chat agent picking up this work.
**Goal:** run a detailed side-by-side comparison of the **current fixed
stack** versus the **pre-fix baseline** (`perf/latest-bulk-prealloc`)
with a performance test first, then a 10-minute stress test. Clean up
old session data before starting.

---

## Branches at play

| Branch | Tip | What it is |
|---|---|---|
| `fix/marshal-source-2026-04-18` | `b1519b2` | **Current fixed stack.** 21-bug audit + fixes + real HIGH #10 backpressure. Lives in `.worktrees/mri_data_marshal/`. |
| `fix/marshal-bug-audit-2026-04-18` | `df3d45b` | Umbrella ops/docs (compose defaults, bench_fps dedup, DEVELOPER_GUIDE refresh). Umbrella only. |
| `perf/latest-bulk-prealloc` | `9b4599a` | **Pre-fix baseline.** Bulk-prealloc experiment + the one real viz_client probe-read commit (`9b4599a`, which is the revert-promise that commit `dd726e7`/`1822829`-era claimed but didn't land). No audit fixes. |

**Both branches have the fixed viz_client probe-read.** Perf branch has
it at `9b4599a`. Fix branch inherits it through the rebase + carries the
bug fixes on top. So the viz_client H5 crash that bit during the first
bench session is NOT in scope here — both branches read H5 cleanly.

---

## What to compare

Run the **same** demo end-to-end on both branches and report:

### 1. Performance baseline (short run)
- `DURATION=30s`
- `KSPACE_INTERVAL=0.025` (40 Hz scanner target)
- Report: mean / median / min / max / stdev FPS from `bench_fps.sh`
- Report: mock_recon "acquisitions received" count over the run
- Report: kspace_streamer "images received back" count

### 2. Stress test (long run)
- `DURATION=600s` (10 min)
- `KSPACE_INTERVAL=0.025` (same 40 Hz)
- Report same FPS + count metrics
- Also report: any new WARN/ERROR lines in `marshal.log` that did not
  appear in the 30s run (look for: session dropped, enqueue dropped,
  scanner disconnected, reject logs)
- Check: does memory grow unboundedly? Run
  `ps -o rss= -p $(pgrep marshal)` every 60s during the 10 min. Report
  max RSS.

### 3. Head-to-head table
At end, produce a markdown table:

| Metric | `perf/latest-bulk-prealloc` | `fix/marshal-source-2026-04-18` | Delta |
|---|---|---|---|
| Mean FPS (30s) | | | |
| Mean FPS (10min) | | | |
| Stdev FPS (10min) | | | |
| Min FPS (10min) | | | |
| Max RSS over 10min | | | |
| Acquisitions/sec sustained | | | |
| Warnings in log (10min) | | | |
| Crashes / exits | | | |

Goal: confirm **fix branch does not regress performance** (expect same
or slightly better) AND **sustains it over 10 min without memory
growth or new warnings**.

---

## How to clean before starting

**Old session data from prior development** lives in the umbrella and
the inner worktree. Clean all of it before the first test run so the
benches start with an empty disk footprint:

```bash
# In umbrella
cd /workspaces/cwru_data_marshal
sudo rm -rf session-data/live/* session-data/dump/* \
            session-data/from_reconstruction/* session-data/from_scanner/* 2>/dev/null || true
sudo rm -rf session-data/run_* 2>/dev/null || true

# In inner worktree (bench_fps.sh writes here)
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal
sudo rm -rf session-data/live/* session-data/dump/* 2>/dev/null || true

# /tmp scratch from prior bench_fps.sh runs
rm -rf /tmp/bench_fps.* /tmp/bench_fps_logs.* 2>/dev/null || true

# Verify
df -h / | head
du -sh /workspaces/cwru_data_marshal/session-data/ 2>/dev/null
du -sh /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/session-data/ 2>/dev/null
ls /tmp/bench_fps* 2>/dev/null
```

**Why first:** prior bench runs filled `/tmp/bench_fps_logs.*` with
dozens of leftover runs (43+ per earlier audit), and stale H5 files in
`session-data/` confuse `viz_client`'s mtime-based change detection.

---

## How to switch between branches

Inner worktree on the same path. Do NOT open a fresh clone — the
branches alias the same build tree and overwriting `latest` tags
would complicate comparison.

```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal

# Start on the fixed branch — it's already the current state
git status                                    # should be clean
git log -1 --format="%h %s"                   # should be b1519b2 (fix)

# For the baseline run:
git stash --include-untracked                 # clean if anything modified
git checkout perf/latest-bulk-prealloc        # 9b4599a
cmake --build build 2>&1 | tail -3            # ~30s incremental rebuild
# Run bench (see "How to run" below)

# Back to fixed:
git checkout fix/marshal-source-2026-04-18    # b1519b2
cmake --build build 2>&1 | tail -3
# Run bench
```

Both branches have the same set of test binaries; the `perf/*` branch
doesn't have the fix-branch's new tests (`test_scanner_pushback`,
`test_scanner_race`, etc.), but the benches only need the 4 runtime
binaries: `marshal`, `kspace_streamer`, `mock_recon.py`, `viz_client`.

---

## How to run

The bench harness is in the inner worktree at
`scripts/bench_fps.sh`. It spawns all 4 host-native processes:
- `mock_recon.py` (Python, port 29002)
- `marshal` (C++, HTTP 28080, MRD TCP 29100)
- `kspace_streamer` (C++)
- `viz_client` (C++)

Usage:
```bash
cd /workspaces/cwru_data_marshal/.worktrees/mri_data_marshal

# Perf run (30s)
DURATION=30 KSPACE_INTERVAL=0.025 ./scripts/bench_fps.sh 2>&1 | tee /tmp/bench_perf.log

# Stress run (10 min)
DURATION=600 KSPACE_INTERVAL=0.025 ./scripts/bench_fps.sh 2>&1 | tee /tmp/bench_stress.log

# Inspect logs (kept in /tmp/bench_fps_logs.*):
latest=$(ls -td /tmp/bench_fps_logs.*/ | head -1)
echo "$latest"
tail /tmp/bench_fps_logs.*/*.log
```

To track RSS during the stress run, open a second shell:
```bash
while pgrep -x marshal >/dev/null; do
  date +%s; ps -o rss= -p $(pgrep -x marshal); sleep 60
done | tee /tmp/marshal_rss.log
```

---

## Reporting format

Write results into:
```
/workspaces/cwru_data_marshal/docs/MRI_MARSHAL_PERF_STRESS_COMPARE_2026-04-19.md
```

Structure:
1. **Setup** — commit SHAs actually tested, host info (`uname -a`, RAM,
   CPU), disk free at start.
2. **Perf results** (30s each) — the head-to-head table above, top 3
   rows only.
3. **Stress results** (10 min each) — full table, plus log-diff
   (warnings that appeared in 10min but not 30s).
4. **Verdict** — does the fix branch maintain performance? Any new
   warnings from the backpressure / session-registry / bounded-queue
   paths firing under load? Does memory stay bounded?
5. **Raw artifacts** — keep `/tmp/bench_fps_logs.*` directories for
   both runs; copy them into `session-data/bench_archive/` so the
   numbers are reproducible from the repo afterward.

---

## What to watch for on the fix branch under 10-min stress

These are the first places the audit fixes could show behavior under
load:

1. **HIGH #4 scanner-writer queue drops** — look for
   `Scanner writer queue full` in `marshal.log`. Cap is 1024 jobs.
2. **HIGH #10 DumpEnqueueResult drops** — `DUMP drop at enqueue time`
   once per kind. (Only fires if `--dump` is enabled. Bench does NOT
   pass `--dump` so this shouldn't appear — if it does, investigate.)
3. **MEDIUM #14 LatestImageWriter coalesce** — no user-visible log, but
   if drops fire, `LatestImageWriter queue exceeded 64 jobs`.
4. **MEDIUM #15 LiveImageRecorder drops** —
   `LiveImageRecorder queue exceeded 4096 jobs`.
5. **HIGH #5/#6/#7 wire-guard rejects** — `IMAGE rejected:` / `TEXT
   rejected:` would mean the kspace_streamer is producing over-cap
   frames (shouldn't happen with mock_recon generating normal-sized
   output).

Any of these firing = real-world signal, not a bug per se. Report them.

---

## Open questions the next agent should answer

1. **Does the fix branch actually match `perf/latest-bulk-prealloc`
   on FPS?** Previous bench rounds showed 40–45 FPS for both, but
   those were 15–30 s runs. Confirm under 10 min.
2. **Any log line that only shows up under sustained load** on the
   fix branch that wasn't there on perf? Those are the new
   observability signals (codex blocker-1 fix).
3. **Is there a measurable FPS cost from the extra return-value
   checks / wire_guards per frame?** Expected answer: no (each is
   O(1) integer compare). Verify.
4. **If the fix branch drops anything** (queue caps), does the
   pipeline recover cleanly or hang? Relevant because codex blocker-1
   just gave us real drop semantics.

---

## Not in scope of this handoff

- Merging either branch back to its parent.
- Rewriting `perf/latest-bulk-prealloc` with the fixes. The two
  branches are kept separate on purpose for exactly this comparison.
- Touching the umbrella fix branch. Compose defaults are already
  correct (`KSPACE_INTERVAL=0.033` set); you are running with
  `0.025` to push harder.
- Any docker compose runs. This is all host-native via `bench_fps.sh`
  to keep the comparison clean.

---

## Current repo state snapshot (for sanity)

Source branches (inner worktree, `.worktrees/mri_data_marshal/`):
- Tip of `fix/marshal-source-2026-04-18`: `b1519b2`
  (fix: HIGH #10 real backpressure via `DumpEnqueueResult`)
- Tip of `perf/latest-bulk-prealloc`: `9b4599a`
  (viz_client probe-read revert — what 1822829-era promised)

Umbrella (`/workspaces/cwru_data_marshal/`):
- Tip of `fix/marshal-bug-audit-2026-04-18`: `df3d45b`
- Tip of `audit/mri-marshal-protocol-fixes-umbrella`: `618d1b1`

Test suite on fix branch: 12 binaries, 4269 assertions (test_dump_overflow
flood loop contributes ~4111 per run; count varies ±a few by timing but
is always above 4000).

---

## One-line summary for your brain-load

> Clean `/tmp/bench_fps_logs.*` and `session-data/*`, run 30 s bench on
> both `perf/latest-bulk-prealloc` (9b4599a) and
> `fix/marshal-source-2026-04-18` (b1519b2), then 10 min stress on
> both, produce comparison table + verdict at
> `docs/MRI_MARSHAL_PERF_STRESS_COMPARE_2026-04-19.md`.
