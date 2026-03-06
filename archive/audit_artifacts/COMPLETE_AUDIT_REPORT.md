# Complete Documentation Audit - ALL Errors Found

**Date:** 2026-01-29
**Auditor:** AI Agent (redo after missing errors)
**Status:** COMPREHENSIVE AUDIT - ALL ERRORS NOW DOCUMENTED

---

## Critical Errors Found

### Error 1: Fake Endpoint `/v1/mrd/stream/{id}` ❌ DOES NOT EXIST

**Location:** [SYSTEM_DIAGRAM_COMPLETE.md:134](SYSTEM_DIAGRAM_COMPLETE.md#L134)

**What the doc says:**
```
│ /v1/mrd/stream/{id}  │ GET  │ Get specific stream info  │
```

**Reality:** **THIS ENDPOINT DOES NOT EXIST IN THE CODE**

**Verified against [marshal_http.hpp](/.worktrees/mri_data_marshal/src/marshal_http.hpp):**

All actual GET endpoints:
- Line 146: `GET /health`
- Line 153: `GET /v1/pose/current`
- Line 320: `GET /v1/bio/latest`
- Line 341: `GET /v1/config`
- Line 527: `GET /v1/mrd/frame?path=...&index=...`
- Line 612: `GET /v1/mrd/ingest?path=...`
- Line 675: `GET /v1/mrd/latest`
- Line 698: `GET /v1/mrd/since?ts=...&limit=...&last=...`

❌ **No `/v1/mrd/stream/{id}` endpoint exists**

**Also appears incorrectly in:**
- [SYSTEM_DIAGRAM_COMPLETE.md:103](SYSTEM_DIAGRAM_COMPLETE.md#L103) - in ASCII diagram

**Impact:** MAJOR - Completely fabricated endpoint

---

### Error 2: Fake Endpoint `/v1/mrd/reconstruction/status` ❌ NOT YET IMPLEMENTED

**Locations:**
- [MRI_MARSHAL_QUICK_OVERVIEW.md:200](MRI_MARSHAL_QUICK_OVERVIEW.md#L200)
- [MRI_MARSHAL_RECONSTRUCTION_ROUTING.md:525-540](MRI_MARSHAL_RECONSTRUCTION_ROUTING.md#L525-L540)

**What the docs say:**
```bash
GET /v1/mrd/reconstruction/status/{id}
# Returns reconstruction status
```

**Reality:** **THIS ENDPOINT DOES NOT EXIST - PHASE 2 NOT IMPLEMENTED**

These docs describe Phase 2 functionality that hasn't been implemented yet. The document should clearly mark these as "FUTURE" or "NOT YET IMPLEMENTED".

**Impact:** MAJOR - Describes unimplemented features as if they exist

---

### Error 3: Wrong HTTP Status Code for IMAGE → /ingest ❌

**Locations:**
- [HTTP_ROUTING_EXAMPLES.md:332](HTTP_ROUTING_EXAMPLES.md#L332)
- [HTTP_ROUTING_EXAMPLES.md:538](HTTP_ROUTING_EXAMPLES.md#L538)

**What the docs say:**
```
IMAGE → /ingest: HTTP 200 OK
```

**Reality from code ([marshal_http.hpp:516](/.worktrees/mri_data_marshal/src/marshal_http.hpp#L516)):**
```cpp
// Process the data (works for both HDF5 files and single frames)
auto entry = mrd::ingest_payload(state, body.data(), body.size(), "http");
return make_response(http::status::created, entry);  // ← HTTP 201 Created
```

**Correct status:** `HTTP 201 Created` (not 200 OK)

Both IMAGE and HDF5 sent to `/ingest` return **201 Created**, not 200 OK.

**Impact:** MODERATE - Wrong HTTP status code documented

---

### Error 4: "Redirect" Terminology (Already Fixed)

**Status:** ✅ FIXED in previous audit
- Changed "redirect" to "internal forward" in 4 places
- Verified against code showing direct function call

---

## Complete List of Actual Endpoints

Verified from [marshal_http.hpp](/.worktrees/mri_data_marshal/src/marshal_http.hpp):

### Health & Config
- `GET /health` (line 146)
- `GET /v1/config` (line 341)

### Pose Tracking
- `GET /v1/pose/current` (line 153)
- `POST /v1/pose/update` (line 176)

### Biological Signals
- `POST /v1/bio/signal` (line 259)
- `GET /v1/bio/latest` (line 320)

### MRI Data
- `POST /v1/mrd/frame` (line 347)
- `POST /v1/mrd/ingest` (line 462)
- `GET /v1/mrd/frame?path=...&index=...` (line 527)
- `GET /v1/mrd/ingest?path=...` (line 612)
- `GET /v1/mrd/latest` (line 675)
- `GET /v1/mrd/since?ts=...&limit=...&last=...` (line 698)

### ❌ Do NOT exist:
- `/v1/mrd/stream/{id}` - **FAKE**
- `/v1/mrd/reconstruction/status/{id}` - **NOT YET IMPLEMENTED**

---

## Correct HTTP Status Codes

Verified from actual code:

| Endpoint | Data Type | Status Code | Code Line |
|----------|-----------|-------------|-----------|
| POST /v1/mrd/frame | IMAGE | 200 OK | 453 |
| POST /v1/mrd/frame | HDF5 | 201 Created | 386 |
| POST /v1/mrd/frame | ACQUISITION (no recon) | 501 Not Implemented | 373 |
| POST /v1/mrd/frame | UNKNOWN | 400 Bad Request | 398 |
| POST /v1/mrd/ingest | IMAGE | **201 Created** | 516 |
| POST /v1/mrd/ingest | HDF5 | 201 Created | 516 |
| POST /v1/mrd/ingest | ACQUISITION (no recon) | 501 Not Implemented | 483 |
| POST /v1/mrd/ingest | UNKNOWN | 400 Bad Request | 506 |

**Key finding:** `/v1/mrd/ingest` returns `201 Created` for BOTH IMAGE and HDF5, not different status codes.

---

## Required Corrections

### Fix 1: Remove fake `/v1/mrd/stream/{id}` endpoint

**File:** [SYSTEM_DIAGRAM_COMPLETE.md](SYSTEM_DIAGRAM_COMPLETE.md)

**Line 103:** Remove from ASCII diagram
**Line 134:** Remove from endpoint table

### Fix 2: Mark reconstruction endpoints as "NOT YET IMPLEMENTED"

**Files:**
- [MRI_MARSHAL_QUICK_OVERVIEW.md](MRI_MARSHAL_QUICK_OVERVIEW.md)
- [MRI_MARSHAL_RECONSTRUCTION_ROUTING.md](MRI_MARSHAL_RECONSTRUCTION_ROUTING.md)

Add clear warnings:
```markdown
⚠️ **NOT YET IMPLEMENTED** - This endpoint is part of Phase 2 (pending)
```

### Fix 3: Correct HTTP status code for IMAGE → /ingest

**File:** [HTTP_ROUTING_EXAMPLES.md](HTTP_ROUTING_EXAMPLES.md)

**Line 332:** Change `HTTP/1.1 200 OK` to `HTTP/1.1 201 Created`
**Line 538:** Change `200 OK` to `201 Created`

---

## Files That Need Correction

1. ❌ [SYSTEM_DIAGRAM_COMPLETE.md](SYSTEM_DIAGRAM_COMPLETE.md) - Remove fake endpoint
2. ❌ [HTTP_ROUTING_EXAMPLES.md](HTTP_ROUTING_EXAMPLES.md) - Fix HTTP status code
3. ⚠️ [MRI_MARSHAL_QUICK_OVERVIEW.md](MRI_MARSHAL_QUICK_OVERVIEW.md) - Mark as future
4. ⚠️ [MRI_MARSHAL_RECONSTRUCTION_ROUTING.md](MRI_MARSHAL_RECONSTRUCTION_ROUTING.md) - Mark as future

---

## Previous Audit Was Incomplete

**What was missed:**
1. ❌ Fake `/v1/mrd/stream/{id}` endpoint
2. ❌ Wrong HTTP status code for IMAGE → /ingest (said 200, actually 201)
3. ⚠️ Unimplemented endpoints not clearly marked as future

**Apologies for the incomplete audit. This is now the complete list of ALL errors.**
