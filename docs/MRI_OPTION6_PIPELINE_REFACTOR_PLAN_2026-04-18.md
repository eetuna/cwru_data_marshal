# MRI Option 6 — Pipeline Refactor and Implementation Plan

## Purpose

This document extracts the previously discussed **Option 6** into one place.

Option 6 was not one of the five already-written options in
[docs/MRI_LATEST_IMAGE_DIAGNOSIS_AND_OPTIONS_2026-04-16.md](MRI_LATEST_IMAGE_DIAGNOSIS_AND_OPTIONS_2026-04-16.md).
It is the larger follow-on idea that came up after those options: a
**pipeline/scheduling refactor** intended to reduce hot-path contention without
changing the frozen MRI marshal contract.

This is a design and implementation-plan document only.
It is **not** a claim that Option 6 is already proven.

## Short Definition

Option 6 means:

- keep the scanner-facing MRD TCP behavior unchanged
- keep the recon-facing MRD TCP behavior unchanged
- keep the HTTP latest-image contract unchanged
- keep per-scan reconstruction archival outputs unchanged at the contract level
- but internally **separate recon/scanner forwarding from storage work**
- and centralize recon-result storage decisions in **one ordered storage executor**

In plain language:

> do less work inline on the latency-sensitive recon/scanner path, and move
> result assembly/materialization into one place that owns latest publication
> and archival side effects in a controlled order

## Why Option 6 Exists

The current diagnosis already established:

- latest-file rename/promotion is probably not the main bottleneck
- queue growth alone is probably not the main bottleneck
- the strongest remaining signal is duplicated recon-side HDF5 append churn

The five written options mostly focus on **what artifact becomes the source of
truth**.
Option 6 focuses on **where and when work happens**.

It exists because even if the final artifact choice is correct, the marshal may
still perform too much storage-related work directly on the hot path.

## What Option 6 Is Trying To Improve

Option 6 is trying to improve the following behavior:

- recon return messages should be forwarded to the scanner promptly
- latest publication should not require avoidable duplicate storage work inline
- archival/live-history work should happen in a predictable serialized pipeline
- ownership of "this logical recon result is complete; now publish/materialize it"
  should live in one place instead of being spread across multiple ad hoc steps

## What Option 6 Is Not

Option 6 is **not**:

- another temp-file naming tweak
- another slot-path variation
- another small queue-depth/backpressure experiment
- a client-side optimization plan
- a proposal to change the MRI protocol contract

It is also **not automatically the same as Option 1**.

Option 1 is mainly about making one closed artifact the source of truth.
Option 6 is about restructuring the pipeline so that storage/materialization is
owned by one executor and is no longer intertwined with the scanner/recon hot
path.

Those ideas can be combined, but they are not identical.

## Contract Constraints

Option 6 must stay compatible with the frozen contract in:

- [docs/MRI_MARSHAL_PROTOCOL_CONTRACT.md](MRI_MARSHAL_PROTOCOL_CONTRACT.md)
- [docs/RECONSTRUCTION_INTERFACE.md](RECONSTRUCTION_INTERFACE.md)

That means Option 6 must preserve these externally visible behaviors:

- one MRD TCP connection from marshal to recon per scan
- recon return messages still flow back to the scanner on the scanner TCP
  connection
- per-scan recon data still ends up archived under `live/from_reconstruction`
- `GET /image/latest` still returns a path to a closed artifact for dumb pollers
- recon failure behavior remains unchanged

## Current Pipeline, Simplified

Today, the MRI path conceptually does something like this:

1. scanner sends MRD traffic to marshal
2. marshal forwards relevant scanner traffic to recon
3. recon sends image messages back to marshal
4. marshal forwards scanner-relevant messages back to the scanner
5. marshal also performs storage-related work for live recon history and latest
   publication
6. latest publication and archival concerns are closely coupled to the incoming
   recon message handling path

The main risk in this structure is that the path handling recon output also owns
multiple storage side effects.

## Proposed Option 6 Pipeline

Option 6 introduces a harder separation of responsibilities.

### Stage A — transport path

The transport path does only transport-critical work:

- receive recon return messages
- immediately forward scanner-relevant return messages to the active scanner
  connection in protocol order
- copy only the minimum metadata/payload needed for later storage processing
- enqueue storage work rather than performing full materialization inline

### Stage B — result assembler

A result assembler groups incoming recon images into one logical result using the
same completion rules already embodied in the live-image grouping logic.

Its job is to decide:

- which incoming images belong to the same logical result
- when that logical result is complete
- what buffered representation is sufficient to hand off for storage

### Stage C — single storage executor

A single ordered storage executor owns all post-group storage actions for that
logical result.

Its responsibilities:

- write or update the per-scan recon archive representation
- materialize the latest closed artifact
- perform promotion/swap of the latest pointer target
- emit timing/cadence diagnostics around these actions
- guarantee ordering so later results do not partially overtake earlier ones in
  storage state

This executor is the center of Option 6.

## Core Design Principle

The key principle is:

> transport threads should move messages; storage executor should own disk work

That does **not** necessarily mean all work becomes asynchronous and unordered.
It means storage side effects are deliberately serialized in one place, while the
transport-facing path stops doing avoidable file work directly.

## How Option 6 Relates To The Existing Code

The earlier diagnosis and experiments point to these likely touchpoints in the
MRI worktree/branches:

- `src/recon_forwarder.hpp`
- `src/live_image_store.hpp`
- `src/live_image_recorder.cpp`
- `src/live_image_recorder.hpp`
- `src/latest_image_writer.cpp`
- `src/latest_image_writer.hpp`
- `src/marshal_state.hpp`
- `tests/test_http_handlers.cpp`
- integration tests around latest publication and recon archive behavior

Conceptually:

- `recon_forwarder` should become thinner on storage responsibilities
- `live_image_store` should remain or evolve into the logical-result assembler
- `latest_image_writer` should become an executor-owned sink, not an ad hoc side
  effect triggered from multiple places
- archival writing should be scheduled alongside latest publication, with one
  owner deciding order and lifecycle

## Design Variants Inside Option 6

Option 6 still leaves one important choice open.

### Variant 6A — executor writes both archive and latest

For each completed logical result:

- executor updates the recon archive representation
- executor also materializes latest

This keeps one owner for all storage work.
The downside is that duplicate writes may still exist unless archive/latest are
further unified.

### Variant 6B — executor materializes one source artifact, derives others

For each completed logical result:

- executor materializes exactly one primary artifact
- latest and archive are then promoted/derived/projected from that artifact

This is structurally closer to Option 1 and is likely the cleaner end-state if
contract semantics allow it.

### Variant 6C — executor makes latest synchronous, archive deferred

For each completed logical result:

- executor materializes latest immediately
- archive projection happens later on the same executor or a lower-priority
  archival stage

This is the most latency-friendly, but only acceptable if archive timing
semantics can be relaxed.

## Recommended Interpretation

The best interpretation of Option 6 is:

- use the pipeline refactor first to create one clear owner of storage work
- then decide whether that owner implements Variant 6A, 6B, or 6C

This keeps the refactor from being blocked on the final artifact policy.

## Implementation Plan

Implementation should be phased.
Do **not** try to land Option 6 as one giant rewrite.

### Phase 0 — freeze the baseline and add minimal measurement hooks

Goal:
- preserve a known baseline before structural changes

Tasks:
- branch from the V0 MRI baseline used in the latest-image investigation
- keep current timing/cadence metrics that were already informative
- add only the measurements needed to separate:
  - recon receive time
  - scanner forward time
  - storage enqueue time
  - storage executor service time
  - logical-result completion-to-latest-publish latency

Success criteria:
- measurements clearly distinguish transport-path time from executor-owned time

### Phase 1 — introduce a storage-executor abstraction without changing behavior

Goal:
- create the seam first

Tasks:
- add a `StorageExecutor`-style component that accepts ordered jobs
- keep the current storage behavior functionally the same at first
- route latest publication through the executor
- preserve current archive/live-history behavior

Likely files:
- `src/latest_image_writer.*`
- `src/marshal_state.hpp`
- recon/live-image orchestration code in the MRI worktree

Success criteria:
- behavior is unchanged from the user's point of view
- tests still pass
- latest publication is now owned by one explicit executor path

### Phase 2 — move logical-result completion handoff behind one owner

Goal:
- stop triggering storage side effects from multiple places

Tasks:
- make the logical result assembler produce a single "result complete" handoff
- ensure grouped recon images are passed to the executor in one package
- remove any duplicate direct latest-write triggers from the recon receive path

Likely files:
- `src/live_image_store.hpp`
- `src/live_image_recorder.*`
- `src/marshal_state.hpp`

Success criteria:
- one completion event per logical result
- one executor submission per logical result
- no duplicate latest publication triggers

### Phase 3 — thin the recon/scanner hot path

Goal:
- keep transport responsive

Tasks:
- ensure recon return messages are forwarded to the scanner before any heavy
  storage work
- restrict inline work to minimal validation/copying/grouping
- move file opens/writes/materialization fully behind the executor boundary

Likely files:
- `src/recon_forwarder.hpp`
- MRI session/message pump code

Success criteria:
- scanner-visible return ordering is preserved
- transport path no longer performs heavy storage work inline

### Phase 4 — choose the artifact policy inside the executor

Goal:
- decide whether archive/latest remain dual-write or become unified

Tasks:
- start with the least risky executor-owned behavior that preserves semantics
- then evaluate whether to move from Variant 6A to 6B or 6C
- only change artifact ownership after Phase 1–3 are working and measurable

Success criteria:
- architecture decision is based on measured executor behavior, not guesswork

### Phase 5 — validation and rollback safety

Goal:
- keep the refactor safe

Tasks:
- validate with focused latest-image tests
- validate recon archive correctness for multislice and grouped images
- validate failure behavior when recon disconnects mid-scan
- validate that `GET /image/latest` still returns correct closed artifacts
- preserve a branchable rollback point after each phase

## Validation Checklist

Option 6 should not be considered successful unless it preserves all of the
following:

- latest publication still works for dumb pollers
- per-scan recon archive output remains correct
- scanner still receives recon return messages in order
- recon failure still surfaces correctly to scanner and HTTP clients
- no regressions in multislice grouping semantics
- no growth of unbounded in-memory state under steady recon traffic

Performance validation should specifically compare:

- V0
- Option 6 Phase 1 seam-only refactor
- Option 6 Phase 3 thinned transport path
- final chosen executor artifact policy

## Risks

Main risks:

- moving too much logic at once and losing correctness
- accidentally changing scanner-visible ordering semantics
- introducing hidden buffering that only relocates the bottleneck
- preserving duplicate writes even after the refactor, which would reduce the
  value of the change
- making archive timing semantics ambiguous

## Recommendation

If work on latest-image performance resumes, Option 6 should be treated as a
**serious architectural candidate**, but only after acknowledging two things:

- it is larger and riskier than the already-tried publication tweaks
- it is still a hypothesis until transport-vs-storage timing is measured after
  the seam is introduced

The most practical way to proceed is:

1. introduce the executor seam first
2. move logical-result completion behind one owner
3. thin the transport path
4. only then decide whether archive/latest should stay dual-write or unify

## One-Line Summary

Option 6 is a repo-specific MRI marshal refactor that keeps the external
protocol contract the same while moving recon-result storage/materialization out
of the transport hot path and under one ordered storage executor.