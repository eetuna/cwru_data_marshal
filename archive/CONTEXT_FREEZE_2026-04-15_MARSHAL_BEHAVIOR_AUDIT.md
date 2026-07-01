# Context Freeze — 2026-04-15 — Marshal Behavior Audit

This file freezes the agreed context and scope for the current MRI marshal audit so future work can refer back to it without re-deriving assumptions.

## Purpose

The goal is to audit the **actual current codebase** against the **desired end-to-end behavior**.

This freeze exists because prior discussion drifted into stale docs, old code, and overgeneralized assumptions.

## Authority Order

When auditing or proposing fixes, use this priority order:

1. **Desired end-to-end behavior** in the intended architecture
2. **Actual current code** in this repo
3. Placeholder clients/services only as evidence of current assumptions
4. Docs, old handoffs, and old commits only as non-authoritative historical context

Do **not** treat stale docs or prior commits as the target behavior.

## Intended End-to-End Architecture

### Scanner side
- Scanner behavior is intended to mimic an **ISMRMRD C API style client**.
- Scanner sends MRD TCP messages using the standard framing and object semantics.

### Marshal side
- Marshal is an **MRD TCP peer/proxy + archive point + live/latest publisher**.
- Marshal should preserve transport compatibility between scanner and recon.
- Marshal is **not** supposed to invent reconstruction policy, cadence policy, or arbitrary batching semantics.

### Recon side
- Recon behavior is intended to mimic a **python-ismrmrd-server / ismrmrd-python style reconstruction service**.
- Recon is the layer that interprets incoming scan data and produces meaningful MRD image outputs.
- Therefore, by the time marshal receives recon-returned `IMAGE` messages, those images already reflect reconstruction-side grouping/semantics.

## Desired Behavior

### Transport behavior
- Scanner ↔ marshal ↔ recon uses MRD TCP transparently.
- Marshal forwards scanner-side MRD traffic to recon without changing its meaning.
- Marshal forwards recon-return messages back to the scanner connection when appropriate.

### Exact archival behavior
- Every incoming MRD object should be handled exactly once on ingress.
- Per-scan archive/history should preserve what actually arrived.
- Archive/history should not synthesize fake regroupings or silently rewrite recon outputs.

### Live/latest behavior
Marshal must publish the **latest logical recon result**, not merely the last transport packet.

That means:
- **Standalone 2D**: live/latest should expose that single 2D result.
- **2D multislice**: if recon emits multiple related 2D images that together form the current result, live/latest should expose the logical multislice result rather than only the last slice packet.
- **True 3D**: if recon emits a true 3D MRD image, live/latest should preserve it as a 3D image/volume rather than pretending it is a set of unrelated 2D packets.

### Responsibility boundary
- Recon handles raw acquisition → reconstruction semantics.
- Marshal should not try to reconstruct intent upstream of recon.
- But marshal **does** need to preserve the returned recon image structure well enough that live/latest matches the logical recon output.

## Critical Clarification

A prior overgeneralized claim was wrong:
- It is **not** correct to say marshal cannot know enough to distinguish anything meaningful.

Correct statement:
- If you inspect one isolated archived 2D image out of context, intent may be ambiguous.
- But on the **live recon → marshal path**, marshal sees the returned MRD image stream, including returned image headers/context and sequencing from recon.
- Therefore the real problem is not impossibility in principle; it is whether current marshal code preserves or destroys the logical result boundary implied by recon’s returned MRD images.

## What Is Not the Source of Truth

The following are **not** authoritative targets for desired behavior:
- stale docs
- old handoff notes
- old commits such as `ed2f4e1`
- current `viz_client` behavior
- current `mock_recon` behavior

They are only evidence for how the current implementation behaves or what assumptions it currently encodes.

## Current Known Code Reality (already confirmed from code)

These points were confirmed from current source inspection and should be treated as current-state facts unless code changes later:

### Marshal live/history path
- Per-scan live history stores exact incoming MRD images in the live per-scan HDF5 file.
- Current `/image/latest` publication writes a closed companion HDF5 snapshot containing **exactly one incoming image at a time**.
- Current latest writer processes queued updates FIFO.

### Marshal state
- Current code no longer contains the earlier XML-derived `expected_slices` / slice-buffer state.
- Current live publication logic no longer performs multislice aggregation.

### Viz placeholder behavior
- Current `viz_client` reads group `image_0` from the latest companion file.
- It interprets multislice as **multiple images inside `image_0`**, sorted by `hdr.slice`.
- It does **not** correctly interpret a single true 3D MRD image as a 3D volume for display.

### Mock recon placeholder behavior
- Current `mock_recon.py` emits **one 2D image per slice**.
- It uses XML-derived slice expectations.
- It does **not** emit a true 3D MRD image.

## Known Mismatch Direction

The earlier marshal change to “always publish only the newest single image” is not sufficient for desired behavior because:
- it can only be correct when the latest logical recon result is exactly one MRD image
- it fails when recon emits multiple related 2D images representing the current multislice result

The older opposite approach (“aggregate by XML z”) was also not trustworthy because it imposed marshal-side assumptions that may not match recon’s actual returned-image structure.

So the real bug is not simply:
- “marshal should aggregate” or
- “marshal should never aggregate”

The real bug is:
- **current marshal lacks a correct notion of the logical result boundary for recon-returned images**.

## Audit Target Going Forward

The audit must answer, from the codebase and intended architecture:

1. What returned MRD image header/context fields and stream patterns define a logical recon result boundary?
2. Does current marshal preserve that boundary in archive/history?
3. Does current marshal preserve that boundary in `/image/latest` publication?
4. Are current placeholder clients/services exposing a marshal bug, encoding their own placeholder assumptions, or both?
5. Which component(s) actually need to change to satisfy desired behavior: marshal only, viz only, mock recon only, or some combination?

## Working Rules For Future Steps

- Do not drift back to docs as authority.
- Do not propose another code change before identifying mismatches against the desired behavior.
- Do not reduce the problem to viz-only or docs-only symptoms.
- Keep the full scanner → marshal → recon → marshal-return path in scope.
- Evaluate standalone 2D, 2D multislice, and true 3D explicitly.

## Immediate Next Step After This Freeze

Continue auditing the actual current codebase end-to-end and then produce a mismatch report against the desired behavior above.
