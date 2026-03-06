# Handoff: Audit All Documentation for Accuracy

**For:** Next AI Agent
**Priority:** HIGH
**Task:** Review and correct all documentation created during this session
**Reason:** Multiple inconsistencies and errors have been found - need thorough audit

---

## Problem Statement

During this session, several documentation files were created explaining the MRI Marshal's smart reconstruction routing feature. However, **multiple errors and inconsistencies** have been discovered:

1. ❌ Used term "redirect" when it's actually "internal forward"
2. ❌ Incorrect backward compatibility reasoning (claimed existing clients used `/ingest` for IMAGE, but they didn't)
3. ❌ Inconsistent descriptions across documents
4. ⚠️ Potentially more errors not yet discovered

**The documentation needs a complete audit and correction.**

---

## Files Created This Session (Need Audit)

### Core Documentation
1. **HTTP_ROUTING_EXAMPLES.md** - Complete HTTP routing examples for all data types
2. **SYSTEM_DIAGRAM_COMPLETE.md** - One-page system diagram with endpoints and flow
3. **ENDPOINT_DESIGN_RATIONALE.md** - Why `/ingest` handles IMAGE defensively
4. **HANDOFF_RECONSTRUCTION_INTEGRATION.md** - Guide for implementing Phase 2 (already existed, may have been modified)

### Modified Files
5. **IMPLEMENTATION_SUMMARY.md** - Summary of what was implemented
6. **MRI_MARSHAL_QUICK_OVERVIEW.md** - Quick 2-minute overview
7. **MRI_MARSHAL_RECONSTRUCTION_ROUTING.md** - Complete technical design
8. **DOCKER_RECONSTRUCTION_GUIDE.md** - Docker deployment guide
9. **README_RECONSTRUCTION_ROUTING.md** - Final summary
10. **docs/MANUAL_TERMINAL_SETUP.md** - Manual terminal setup (added reconstruction note)

---

## Known Errors to Fix

### Error 1: "Redirect" vs "Internal Forward"

**Location:** Multiple files
**Issue:** Documentation says `/v1/mrd/frame` "redirects" HDF5 to `/ingest`
**Reality:** It does an **internal forward** by calling `mrd::ingest_payload()` directly

**Incorrect terminology:**
- "redirect to /v1/mrd/ingest"
- "HTTP redirect"
- "307 redirect"

**Correct terminology:**
- "internal forward to ingest logic"
- "calls ingest handler internally"
- "forwards to ingest processing"

**Files to check:**
- HTTP_ROUTING_EXAMPLES.md
- SYSTEM_DIAGRAM_COMPLETE.md (already partially fixed)
- IMPLEMENTATION_SUMMARY.md
- MRI_MARSHAL_QUICK_OVERVIEW.md
- Any others that mention HDF5 handling in `/frame`

### Error 2: Backward Compatibility Claim

**Location:** ENDPOINT_DESIGN_RATIONALE.md, possibly others
**Issue:** Claimed `/ingest` handles IMAGE for backward compatibility with existing clients
**Reality:** No existing clients send IMAGE to `/ingest` - it's for **defensive programming/robustness**, not backward compatibility

**What to fix:**
- Remove claims about "existing clients using /ingest for IMAGE"
- Clarify the real reason: defensive handling, user-friendliness, not backward compat
- Note: `image_streamer` uses `/v1/mrd/frame` (line 212 of image_streamer_main.cpp)

**Files to check:**
- ENDPOINT_DESIGN_RATIONALE.md (most critical)
- Any file mentioning backward compatibility for `/ingest` + IMAGE

### Error 3: Inconsistent Endpoint Descriptions

**Location:** Various files
**Issue:** Different files describe endpoints differently

**Need to verify consistency across all files:**
- What each endpoint accepts
- What happens with each data type
- HTTP status codes returned
- Storage methods used (SWMR vs complete file)

---

## Audit Checklist

For each documentation file:

### 1. Technical Accuracy

- [ ] **HDF5 handling:** Correctly described as "internal forward", not "HTTP redirect"?
- [ ] **IMAGE to /ingest:** Correctly described as "defensive handling", not "backward compatibility"?
- [ ] **SWMR vs Complete file:** Correct storage method for each endpoint/data type combo?
- [ ] **HTTP status codes:** Accurate (200, 201, 400, 501, 502)?
- [ ] **Flow diagrams:** Match actual implementation?

### 2. Code References

- [ ] **File paths:** Do referenced files exist?
- [ ] **Line numbers:** Are they accurate (if provided)?
- [ ] **Function names:** Match actual code (e.g., `mrd::ingest_payload`)?
- [ ] **Endpoints:** Exactly `/v1/mrd/frame` and `/v1/mrd/ingest` (not variations)?

### 3. Consistency Across Documents

- [ ] **Terminology:** Same terms used across all docs?
- [ ] **Endpoint behavior:** Described identically in all files?
- [ ] **Data type routing:** Consistent routing rules?
- [ ] **Examples:** Use same scenarios/dimensions/values?

### 4. Completeness

- [ ] **All data types covered:** IMAGE, ACQUISITION, HDF5_FILE, UNKNOWN
- [ ] **Both endpoints covered:** /frame and /ingest
- [ ] **Both scenarios:** With and without reconstruction service
- [ ] **Error cases:** 400, 501, 502 documented

### 5. Clarity

- [ ] **No confusing statements:** Check for contradictions
- [ ] **Clear examples:** HTTP requests/responses are complete
- [ ] **Proper formatting:** Markdown renders correctly
- [ ] **Accessible language:** Can a new developer understand it?

---

## How to Verify Accuracy

### Step 1: Read the Source Code

**Critical files to check:**
```bash
# Main HTTP handlers
.worktrees/mri_data_marshal/src/marshal_http.hpp

# Type detection logic
.worktrees/mri_data_marshal/include/mrd_type_detector.hpp

# Existing clients (to verify usage patterns)
.worktrees/mri_data_marshal/clients/image_streamer/image_streamer_main.cpp
```

**Key things to verify:**

1. **HDF5 in /frame (line ~381-387 of marshal_http.hpp):**
   ```cpp
   case mrd::MrdDataType::HDF5_FILE:
       auto entry = mrd::ingest_payload(state, body.data(), body.size(), "http");
       return make_response(http::status::created, entry);
   ```
   → **Internal function call**, not HTTP redirect

2. **IMAGE in /ingest (line ~498+ of marshal_http.hpp):**
   - Does it call SWMR append?
   - Does it add a warning?
   - What HTTP status code?

3. **ACQUISITION handling:**
   - Currently returns HTTP 501?
   - With reconstruction: what happens?

### Step 2: Compare Documentation to Code

For each documented behavior:
1. Find the corresponding code
2. Verify the description matches
3. Note any discrepancies
4. Update documentation

### Step 3: Check Cross-References

**Verify all internal links work:**
```bash
# Find all markdown links
grep -r "\[.*\](.*\.md)" *.md

# Check if targets exist
```

**Verify all code references are accurate:**
```bash
# Example: verify marshal_http.hpp exists
ls .worktrees/mri_data_marshal/src/marshal_http.hpp

# Verify line numbers (if mentioned)
head -n 385 .worktrees/mri_data_marshal/src/marshal_http.hpp | tail -5
```

### Step 4: Test Examples

**If possible, verify example curl commands:**
```bash
# Start marshal
./build/marshal --data ./test-data

# Try examples from documentation
curl -X POST http://localhost:8080/v1/mrd/frame \
  --data-binary @test.bin

# Verify response matches documented response
```

---

## Specific Files to Audit

### Priority 1: Core Technical Docs

**HTTP_ROUTING_EXAMPLES.md**
- [ ] All 7 examples are technically accurate
- [ ] HTTP requests are complete and correct
- [ ] HTTP responses match what marshal actually returns
- [ ] Flow descriptions match code
- [ ] No "redirect" terminology for HDF5 → ingest
- [ ] Storage paths are correct (/frame→SWMR, /ingest→complete file after recon)

**SYSTEM_DIAGRAM_COMPLETE.md**
- [ ] ASCII diagram is accurate
- [ ] Endpoint table shows correct accepts/actions
- [ ] Data type detection table is correct
- [ ] Configuration examples work
- [ ] Storage decision table is accurate

**IMPLEMENTATION_SUMMARY.md**
- [ ] Files modified list is complete
- [ ] Code changes described accurately
- [ ] Examples match actual behavior
- [ ] Detection rules table is correct

### Priority 2: Design/Explanation Docs

**ENDPOINT_DESIGN_RATIONALE.md**
- [ ] Remove/correct backward compatibility claims
- [ ] Verify actual reason: defensive handling for robustness
- [ ] Examples are technically correct
- [ ] Comparisons (current vs hypothetical) are fair and accurate

**MRI_MARSHAL_QUICK_OVERVIEW.md**
- [ ] 2-minute overview is accurate
- [ ] Diagrams match implementation
- [ ] Storage paths are correct

**MRI_MARSHAL_RECONSTRUCTION_ROUTING.md**
- [ ] Complete technical design matches code
- [ ] Phase 1 description is accurate
- [ ] Phase 2 description matches handoff doc

### Priority 3: Guides

**DOCKER_RECONSTRUCTION_GUIDE.md**
- [ ] Docker compose examples work
- [ ] Service configuration is correct
- [ ] Examples match actual implementation

**HANDOFF_RECONSTRUCTION_INTEGRATION.md**
- [ ] Implementation steps are accurate
- [ ] Code examples are correct
- [ ] External service API is properly documented

**docs/MANUAL_TERMINAL_SETUP.md**
- [ ] Reconstruction service note is accurate
- [ ] Links to other docs work

---

## Common Patterns to Look For

### Red Flags (Likely Errors)

1. **"Redirect" when talking about HDF5 → ingest**
   - Should be "internal forward" or "calls ingest_payload()"

2. **"Backward compatibility" for /ingest + IMAGE**
   - Should be "defensive handling" or "robustness"

3. **"/ingest stores IMAGE to complete file"**
   - Wrong: /ingest stores IMAGE to **SWMR** (with warning)

4. **"/frame stores HDF5"**
   - Wrong: /frame **forwards HDF5** to ingest (doesn't store)

5. **"WebSocket notifications in HTTP flow"**
   - Wrong: HTTP is synchronous, WebSocket is separate system

6. **Inconsistent HTTP status codes**
   - IMAGE: 200 OK
   - HDF5: 201 Created
   - ACQUISITION (no recon): 501 Not Implemented
   - ACQUISITION (with recon): 201 Created
   - UNKNOWN: 400 Bad Request
   - Recon service error: 502 Bad Gateway

### Good Signs (Likely Correct)

1. **Clear separation of /frame (streaming) vs /ingest (batch)**
2. **Mentions ingest_payload() function for HDF5**
3. **Explains defensive handling for edge cases**
4. **Shows both with/without reconstruction scenarios**
5. **Complete HTTP request/response examples**

---

## Expected Corrections

### Minimum Changes Needed

1. **Replace "redirect" → "internal forward"** in ~5-10 places
2. **Fix backward compatibility claim** in ENDPOINT_DESIGN_RATIONALE.md
3. **Verify all endpoint behavior tables** match implementation
4. **Check all HTTP status codes** are correct
5. **Verify all storage paths** (SWMR vs complete file)

### If Major Issues Found

If you find fundamental misunderstandings:
1. **Document the issue clearly**
2. **Create corrected version**
3. **Note what was wrong and why**
4. **Update all affected files**
5. **Create summary of all corrections made**

---

## Verification Commands

```bash
# Check implementation files exist
ls .worktrees/mri_data_marshal/src/marshal_http.hpp
ls .worktrees/mri_data_marshal/include/mrd_type_detector.hpp

# Find HDF5 handling code
grep -n "HDF5_FILE" .worktrees/mri_data_marshal/src/marshal_http.hpp

# Find ingest_payload function
grep -n "ingest_payload" .worktrees/mri_data_marshal/src/marshal_http.hpp

# Verify image_streamer uses /frame
grep "v1/mrd" .worktrees/mri_data_marshal/clients/image_streamer/image_streamer_main.cpp

# Check for any "redirect" mentions in docs
grep -i "redirect" *.md | grep -i ingest
```

---

## Output Expected

After audit, create a new file: **AUDIT_CORRECTIONS_SUMMARY.md** with:

### Section 1: Files Audited
- List of all files checked
- Status: ✅ Accurate, ⚠️ Minor fixes, ❌ Major errors

### Section 2: Errors Found
- Description of each error
- Which files contained the error
- What the correct information is
- Code references proving correctness

### Section 3: Corrections Made
- List of all changes made
- Before/after for each correction
- Files modified

### Section 4: Remaining Issues
- Any unresolved questions
- Areas that need clarification
- Suggested improvements

### Section 5: Verification
- How correctness was verified
- Code references checked
- Tests run (if any)

---

## Important Notes

1. **Trust the code, not prior documentation**
   - If documentation contradicts code, code is correct
   - Update documentation to match implementation

2. **Verify claims with evidence**
   - Don't assume prior AI agent was correct
   - Check actual source code
   - Look for actual client usage patterns

3. **Be thorough but efficient**
   - Focus on technical accuracy first
   - Polish and clarity second
   - Consistency third

4. **Document your audit process**
   - Note what you checked
   - Record how you verified
   - List sources of truth used

---

## Success Criteria

Audit is complete when:

- [ ] All files have been reviewed
- [ ] All known errors are fixed
- [ ] All claims are verified against code
- [ ] All cross-references work
- [ ] All examples are accurate
- [ ] Terminology is consistent
- [ ] AUDIT_CORRECTIONS_SUMMARY.md is created
- [ ] No obvious contradictions remain

---

## Questions to Answer

As you audit, answer these:

1. **Does `/v1/mrd/frame` do an HTTP redirect or internal forward for HDF5?**
   - Answer: _______________

2. **Why does `/v1/mrd/ingest` accept IMAGE data?**
   - Answer: _______________

3. **What happens to IMAGE sent to `/ingest`?**
   - Answer: _______________

4. **What happens to HDF5 sent to `/frame`?**
   - Answer: _______________

5. **What HTTP status codes are returned for each scenario?**
   - IMAGE → /frame: _______________
   - IMAGE → /ingest: _______________
   - HDF5 → /frame: _______________
   - HDF5 → /ingest: _______________
   - ACQUISITION (no recon) → either: _______________
   - ACQUISITION (with recon) → /frame: _______________
   - ACQUISITION (with recon) → /ingest: _______________
   - UNKNOWN → either: _______________

6. **Do any existing clients send IMAGE to `/ingest`?**
   - Answer: _______________

---

## Final Note

**This is critical work.** The documentation is the primary reference for:
- Future developers
- Users integrating with the marshal
- The next phase of development (reconstruction integration)

Inaccurate documentation is worse than no documentation, because it misleads people and wastes their time debugging "unexpected" behavior that's actually correct.

**Take your time. Be thorough. Verify everything.**

---

**Good luck, and thank you for fixing my mistakes!**
