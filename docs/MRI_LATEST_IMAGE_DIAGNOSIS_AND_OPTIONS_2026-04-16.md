# MRI Latest Image Diagnosis and Options — 2026-04-16

## Purpose

This document exports the current diagnosis state for the MRI marshal latest-image performance/stability investigation.

It is meant to give another agent or engineer enough context to continue work without re-deriving the problem from scratch.

This document intentionally includes:
- what was tested
- what was reverted
- what was implemented
- what passed and failed
- what measurements were taken
- what those measurements actually prove
- what options remain
- what should and should not be optimized next

## Executive Summary

The latest-image problem is not solved yet.

Two server-side redesigns were implemented and validated functionally:
1. a closed-result-file reuse candidate
2. a fixed-slot latest-result variant

Both were real experiments, both passed focused correctness/integration checks, and both were benchmarked against V0. Neither became a clear performance breakthrough.

Additional targeted instrumentation was then added to the MRI marshal hot path. The resulting measurements show:
- atomic latest-file promotion is essentially free
- queue buildup is not the main problem
- the strongest remaining server-side signal is duplicated recon-side HDF5 append churn

The most plausible remaining root issue is that marshal is still doing repeated small recon HDF5 append work twice on the hot path:
- once for live recon history
- once for the latest-result artifact

That duplicated append cadence appears more important than latest-file rename/promotion mechanics.

## Scope Clarification

### What is the actual goal?

The actual goal is not “make viz-client smart.”

The actual goal is:
- keep the latest-image contract stable for a dumb poller
- improve end-to-end stability and throughput of MRI marshal latest publication under the real reconstruction flow
- avoid making runtime behavior worse than V0
- preserve correctness of latest publication and recon result handling

### Why clients are not the main target

The placeholder status of viz-client does **not** make its instability meaningless.

It means:
- viz-client is a symptom reader, not the product to optimize around
- unstable placeholder behavior still indicates instability in the server publication path
- server-side fixes should not depend on changing clients

Current conclusion:
- no client edits are required for valid testing of the marshal issue
- optional client logging changes may help future measurement, but they are not required for the real fix

## Baselines and Branches

### V0 baseline

- Commit: `837a101`
- Temporary benchmark worktree: `.worktrees/mri_v0_benchmark`

This was treated as the clean baseline for comparison.

### First committed candidate

- Branch: `perf/latest-file-reuse`
- Commit: `f7bb797`
- Commit message: `feat: reuse closed recon result files for latest publication`

This is the first committed experimental candidate.

### Second experimental branch

- Branch: `perf/latest-slot-reuse`
- Code checkpoint commit: `0c30408`
- Commit message: `experiment: add slot-based latest path and cadence diagnostics`

This branch was used for:
- slot-based latest-result experimentation
- temporary instrumentation
- diagnosis runs

Important note:
- `0c30408` is an experiment checkpoint, not a claimed fix
- it captures both the slot-based latest-path variant and the marshal-side timing/cadence diagnostics used in this diagnosis pass

## What Was Confirmed Early

Before these new experiments, one important point was already established:

The earlier bounded-backpressure queue idea was not the fix.

That queue-throttling variant was rolled back because runtime behavior was worse than the earlier baseline. The remaining work focused on the real V0 cost center instead of queue pressure experiments.

## Original V0 Diagnosis

The important V0 observation was:
- recon results were already being appended into live HDF5 history
- latest publication still rebuilt a fresh `latest_image.h5` from images again

That means V0 did repeated HDF5 serialization work for latest publication.

The most relevant V0 code areas are:
- [src/live_image_store.hpp](../.worktrees/mri_v0_benchmark/src/live_image_store.hpp)
- [src/latest_image_writer.cpp](../.worktrees/mri_v0_benchmark/src/latest_image_writer.cpp)
- [src/mrd_sink.cpp](../.worktrees/mri_v0_benchmark/src/mrd_sink.cpp)

The earlier theory was:
- if latest rewrite is expensive, then publishing a closed result artifact instead of rebuilding latest from image vectors should help

That theory was plausible, but it turned out to be incomplete.

## SWMR-Era Inspection: What Was Actually Learned

A code archaeology pass was done on the last pre-removal SWMR generation.

Important finding:
- the remembered “cache” was not a writer-side latest-image cache
- it was primarily metadata caching and reader-side HDF5 caching

Useful takeaway from that historical pass:
- a closed artifact/publish idea is valid conceptually
- but the specific remembered implementation was not the same as the current latest publication problem

This historical inspection was helpful for inspiration, but it did **not** produce a ready-made answer.

## Experiment 1: Closed Result File Reuse Candidate

### Branch / commit

- Branch: `perf/latest-file-reuse`
- Commit: `f7bb797`

### Intent

Reduce or eliminate the need to rebuild `latest_image.h5` from scratch by:
- incrementally building a closed recon result file
- publishing latest by file reuse/promote instead of reserializing a fresh latest file each time

### Core changes

Main changed files:
- [src/latest_image_writer.cpp](../.worktrees/mri_latest_file_reuse/src/latest_image_writer.cpp)
- [src/latest_image_writer.hpp](../.worktrees/mri_latest_file_reuse/src/latest_image_writer.hpp)
- [src/live_image_recorder.cpp](../.worktrees/mri_latest_file_reuse/src/live_image_recorder.cpp)
- [src/live_image_recorder.hpp](../.worktrees/mri_latest_file_reuse/src/live_image_recorder.hpp)
- [src/live_image_store.hpp](../.worktrees/mri_latest_file_reuse/src/live_image_store.hpp)
- [src/marshal_state.hpp](../.worktrees/mri_latest_file_reuse/src/marshal_state.hpp)
- [tests/test_http_handlers.cpp](../.worktrees/mri_latest_file_reuse/tests/test_http_handlers.cpp)

### What it did

It introduced a path where recon results could be written to reusable closed HDF5 files and then published as latest via file reuse/promotion.

### Important correction made during this work

There was a production-vs-test mismatch:
- production always constructs the latest-image writer at startup
- some unit tests had been resetting that writer and then assuming immediate file visibility

That mismatch was fixed at the test/spec layer, not by preserving a test-only runtime path.

### Validation status

Passed:
- `test_mrd_sink`
- `unit_http_handlers`
- `it_http`
- Python integration suite `tests/integration/test_marshal_integration.py`
- Docker end-to-end reconstruction flow

### Short benchmark outcome

Short sample versus V0 suggested:
- V0 roughly around 14.8–15.9 FPS in the sampled viz logs
- candidate mostly around 15.7–15.9 FPS, with one higher spike and one lower dip

Interpretation:
- functionally valid
- may be slightly better than V0 in short samples
- not a decisive performance breakthrough

### Why this candidate is considered half-baked

Even though it was a real and functioning experiment, it was incomplete because it only attacked one layer of the problem.

It addressed:
- “stop rebuilding latest from scratch every time”

It did **not** fully solve:
- overall duplicate recon append work
- archive/live/latest ownership of the result artifact
- whether live history or latest artifact should be the true source of truth

## Experiment 2: Fixed-Slot Latest Result Variant

### Branch

- `perf/latest-slot-reuse`

### Intent

Take the first candidate and remove unbounded numbered latest-result files by reusing only two fixed latest slots.

### Core idea

Instead of files like:
- `*_latest_result_734.h5`
- `*_latest_result_735.h5`
- etc.

Use a bounded pair such as:
- `latest_slot_a.h5`
- `latest_slot_b.h5`

and alternate between them.

### Why it was attempted

The numbered result-file churn looked like unnecessary file-lifecycle noise and a possible contributor to instability.

### Validation status

The slot-based variant passed:
- `test_mrd_sink`
- `unit_http_handlers`
- `it_http`
- Python integration suite

### Short benchmark outcome

It worked functionally, but the short sample did **not** outperform V0 convincingly.

Observed sampled viz behavior was roughly V0-level at best and sometimes worse than the first candidate.

### Important conclusion from this failure

Cleaner file naming and bounded latest-slot lifecycle were **not** enough.

This strongly suggested that latest-file promotion mechanics were not the real center of gravity.

## Hard Diagnosis Phase

After the two redesigns failed to produce a convincing breakthrough, the work shifted from “new ideas” to direct measurement.

### Goal of diagnosis

Determine whether the actual bottleneck was:
- latest-file promotion
- queue buildup
- one expensive HDF5 append
- or cumulative small-work churn

## Instrumentation Added

Temporary targeted instrumentation was added on the experimental branch in:
- [src/live_image_recorder.cpp](../.worktrees/mri_latest_file_reuse/src/live_image_recorder.cpp)
- [src/latest_image_writer.cpp](../.worktrees/mri_latest_file_reuse/src/latest_image_writer.cpp)
- [src/live_image_recorder.hpp](../.worktrees/mri_latest_file_reuse/src/live_image_recorder.hpp)

This instrumentation was later captured in the experimental slot-branch checkpoint commit `0c30408`.

### Timed operations measured

First pass instrumentation measured:
- live history append operation duration
- latest-result append operation duration
- latest publish/promotion duration
- full latest rewrite duration

### Aggregate cadence instrumentation measured

Second pass instrumentation measured per-second windows for the recon live recorder:
- number of live appends/sec
- average live append ms
- number of latest-result appends/sec
- average latest append ms
- number of latest publishes/sec
- average latest publish ms
- queue depth now
- max queue depth during the window

## Instrumented Docker Flow Findings

A short real Docker reconstruction flow was run with the instrumented MRI image.

### Finding 1: latest-file promotion is basically free

Observed timing summary:
- `latest_promote_ms` almost always `0`
- sometimes `1`

Observed matching wrapper timing:
- `latest_publish_ms` almost always `0`
- sometimes `1`

Interpretation:
- atomic promotion of the latest result to `latest_image.h5` is not the main bottleneck

### Finding 2: queue buildup is not the main problem

Aggregate queue observations from cadence logs:
- recorder queue depth usually around `1`
- max queue depth around `2–3`

Interpretation:
- marshal is not obviously falling behind due to runaway queue growth
- backlog does not appear to be the primary explanation for instability

### Finding 3: no single append op looked dramatically expensive

With a coarse threshold, no obvious giant per-op append cost stood out.

Per-op averages in the aggregate cadence windows were small.

Representative cadence ranges:
- live append avg roughly `0.01–0.22 ms`
- latest append avg roughly `0.18–0.61 ms`
- latest publish avg roughly `0.16–1.07 ms`

Interpretation:
- there is no obvious “one syscall takes 20 ms” style smoking gun

### Finding 4: the real signal is repeated duplicated append churn

Representative cadence windows showed approximately:
- `70–93` live recon appends per second
- `70–93` latest-result appends per second
- `12–19` latest publishes per second

This is the strongest current finding.

Interpretation:
- marshal is performing the same class of recon-side HDF5 append work twice at high frequency
- the issue is likely cumulative repeated small work rather than one expensive latest-file handoff

## Current Best Diagnosis

The strongest evidence currently supports this statement:

The real server-side waste is duplicated recon append churn.

More precisely:
- latest-file rename/promotion is cheap
- queue backlog is modest
- the recon hot path repeatedly performs HDF5 append work twice:
  - once into live recon history
  - once into latest-result materialization

This is the most credible remaining explanation for instability/stall behavior in the latest-image path.

## What This Diagnosis Does Not Yet Prove

This work does **not** yet prove:
- the exact proportion of total marshal CPU time spent in each append destination
- whether archive/live/history requirements can be changed without breaking intended semantics
- whether one of the remaining architectural options is definitely the correct final solution

It proves where **not** to focus:
- not latest-file rename/promotion mechanics
- not client rewrites
- not more queue/backpressure tweaks as the main line of attack
- probably not more temp-file naming or slot naming variants

## Client-Side Conclusion

### Are any client edits required for proper testing?

No.

Current clients are sufficient to test marshal-side latest behavior:
- `kspace-streamer` confirms recon round-trip progress
- `mock-recon` confirms recon intake/progress
- `viz-client` provides end-to-end freshness/stability signal as a dumb poller

### Are client edits required for the real fix?

No.

Optional future client logging could help diagnostics, but should not be part of the actual solution path.

## Options Going Forward

Below are the serious remaining server-side options, ordered by conceptual strength.

### Option 1 — Make one closed recon result artifact the source of truth

#### Idea

For each logical recon result:
- materialize one closed HDF5 artifact exactly once
- use that artifact as the primary representation of the result
- publish latest by file-level promotion/hard-link/copy/rename from that artifact
- let archive/history reference or derive from that same artifact later

#### Why it addresses the measured issue

It removes the duplicated recon-side HDF5 append path.

Instead of:
- append recon into live history
- append same recon again into latest-result artifact

it becomes:
- materialize the result once
- reuse it for latest and archive purposes

#### Pros

- directly attacks the strongest measured issue
- keeps latest publication cheap
- avoids duplicated recon-side HDF5 appends
- conceptually clean if per-result artifacts are acceptable

#### Cons

- bigger architectural change
- may conflict with any requirement that the main recon history must be one continuously growing HDF5 file
- archive semantics must be explicitly redefined

#### Best fit if

- per-result closed artifacts are acceptable as the primary representation
- archive/history can be made to reference or derive from those artifacts

### Option 2 — Keep live history primary, but derive latest without a second recon append path

#### Idea

Continue maintaining the live recon history file, but do not append the same recon images into a second latest-result HDF5 artifact.

Instead, find a way to derive latest publication from the live-history side without another append pass.

#### Why it might work

It tries to preserve existing live-history semantics while removing duplicated append work.

#### Pros

- closer to existing structure
- less disruptive if continuous live-history semantics are important

#### Cons

- hard to do cleanly if latest must be a closed isolated artifact
- may be awkward with a continuously growing monolithic HDF5 file
- may still leak complexity or hidden duplicate work

#### Best fit if

- the continuously growing live-history file is a hard requirement
- archive/live semantics cannot be re-centered around per-result artifacts

### Option 3 — Latest-first hot path, archive/history as deferred background projection

#### Idea

Hot path:
- produce only the latest result artifact needed for the latest-image contract

Background path:
- derive archival/live-history output later

#### Why it might work

It removes archive/history concerns from the latency-sensitive latest path.

#### Pros

- likely best hot-path responsiveness
- clean separation between “what the client needs now” and “what we archive for later”

#### Cons

- archive/history is no longer immediate
- changes semantics if history must be synchronous with latest publication
- requires explicit agreement that deferred archive behavior is acceptable

#### Best fit if

- latest responsiveness is the top priority
- archive lag is acceptable

### Option 4 — Spool recon results in a cheaper intermediate form, materialize one HDF5 artifact on completion

#### Idea

Instead of performing HDF5 appends during result assembly:
- buffer recon wire images in memory or a cheap spool structure
- when the logical result completes, build exactly one HDF5 artifact from the buffered result
- use that artifact for latest and possibly archive

#### Why it might work

It avoids repeated high-frequency duplicate HDF5 appends during result construction.

#### Pros

- preserves logical grouping behavior nicely
- avoids a second HDF5 append path during grouping
- simpler hot-path file lifecycle than repeated temp/result files

#### Cons

- introduces buffer/spool lifecycle complexity
- memory usage must be bounded
- still requires clarity on archive/live ownership of the final artifact

#### Best fit if

- results are moderate enough to buffer/spool safely
- group completion is well-defined

### Option 5 — Reduce publish frequency while leaving the duplicate append structure mostly intact

#### Idea

Keep most of the current design but coalesce or rate-limit latest publications.

#### Pros

- smallest code change
- may reduce visible churn quickly

#### Cons

- does not address the measured root issue
- only hides or amortizes symptoms
- likely not a real fix

#### Recommendation

This should not be the primary direction.

## Current Recommendation

If architecture can tolerate it, Option 1 is the best serious path.

Reason:
- the current diagnosis says the real waste is duplicated recon HDF5 append churn
- Option 1 attacks that directly
- the earlier experiments focused too much on publish mechanics instead of eliminating duplicate append work itself

If continuous live-history semantics are a hard requirement, then Option 2 is the conservative next path, but it is structurally harder and less clearly clean.

## What Should Not Be Done Next

Based on current evidence, the following should not be the main next move:
- more queue/backpressure tuning as the primary strategy
- more latest temp-file naming variants
- client-side optimization as a dependency for the fix
- presenting another unmeasured architecture idea as if it is likely correct before gathering runtime evidence

## Immediate Practical State of the Repository

### Committed state worth preserving

- `perf/latest-file-reuse` at `f7bb797`

This is a functioning committed experiment and an important checkpoint.

### Experimental working state

`perf/latest-slot-reuse` no longer has uncommitted experimental changes from this session.

Those slot-path and diagnostic changes were committed as:
- `0c30408` — `experiment: add slot-based latest path and cadence diagnostics`

Those diagnostics were useful for learning, but they are not yet a production-ready change set.

## Suggested Next Step for the Next Agent

The next agent should **not** start by inventing another publication variant.

Instead, the next best move is:
1. accept the current diagnosis that duplicate recon append churn is the strongest signal
2. decide whether live-history semantics are truly hard requirements
3. choose between Option 1 and Option 2 based on that requirement
4. design the next implementation specifically to remove the duplicate recon append path

If more measurement is needed before implementation, the next measurement should quantify aggregate cost split by destination more precisely, but the current evidence is already strong enough to stop spending time on publish-rename mechanics.

## Final Honest Status

At the end of this session:
- the original problem is better understood than before
- two concrete redesigns were implemented and functionally validated
- neither redesign clearly solved the performance/stability goal
- direct measurement substantially improved confidence about where the problem is **not**
- the most credible unresolved issue is duplicated recon append churn inside marshal

That is the best current understanding.
