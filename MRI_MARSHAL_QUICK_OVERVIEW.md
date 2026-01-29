# MRI Marshal: Reconstruction Routing - Quick Overview

**TL;DR:** Add smart detection to MRI Marshal so it can handle both raw k-space data AND reconstructed images automatically.

---

## Current Setup (Today)

```
┌─────────────┐
│ MRI Scanner │
└──────┬──────┘
       │
       │ Raw k-space
       ▼
┌─────────────────┐
│ Reconstruction  │ ◄── Must exist separately!
│ Service         │
│ (Gadgetron/etc) │
└──────┬──────────┘
       │
       │ Reconstructed ISMRMRD images
       ▼
┌─────────────────────────┐
│   MRI Marshal           │
│   (Port 8080)           │
│                         │
│   ✓ Accepts: Recon only │
│   ✗ Rejects: Raw k-space│
└──────┬──────────────────┘
       │
       ▼
┌─────────────┐
│ Viz Clients │
└─────────────┘
```

**Problem:** Marshal doesn't know what to do with raw scanner data.

---

## Proposed Setup (New)

```
┌─────────────┐
│ MRI Scanner │
└──────┬──────┘
       │
       │ Raw k-space OR Reconstructed
       ▼
┌─────────────────────────────────────────────────────────┐
│              MRI Marshal (Enhanced)                     │
│                                                         │
│  ┌───────────────────────────────────────────┐         │
│  │ POST /v1/mrd/frame OR /v1/mrd/ingest     │         │
│  │                                           │         │
│  │       Auto-Detect Data Type               │         │
│  │    ┌───────────┬──────────┬───────┐      │         │
│  │    ▼           ▼          ▼       ▼      │         │
│  │  Raw K       Image     HDF5    Unknown   │         │
│  └───┬───────────┬──────────┬───────┬───────┘         │
│      │           │          │       │                  │
│      │           │          │       └─► HTTP 400       │
│      │           │          │          "Invalid"       │
│      │           │          │                          │
│      │           │          └─► HTTP 201               │
│      │           │             Store as-is             │
│      │           │                                     │
│      │           └─► HTTP 200                          │
│      │              Store to SWMR ───┐                 │
│      │                                │                 │
│      │ HTTP POST                      │                 │
│      └──────────────┐                 │                 │
│                     │                 │                 │
└─────────────────────┼─────────────────┼─────────────────┘
                      │                 │
                      ▼                 │
         ┌──────────────────────┐       │
         │  Reconstruction      │       │
         │  Service (External)  │       │
         │  - Gadgetron         │       │
         │  - Custom service    │       │
         └──────────┬───────────┘       │
                    │                   │
                    │ HTTP 200          │
                    │ Reconstructed     │
                    │ ImageHeader +     │
                    │ pixels            │
                    ▼                   │
         ┌──────────────────────┐       │
         │  Store to SWMR       │       │
         │  (demo.mrd)          │       │
         └──────────┬───────────┘       │
                    │                   │
                    └───────────────────┘
                            │
                            ▼
                   ┌─────────────────┐
                   │  Viz Clients    │
                   │  (Read SWMR)    │
                   └─────────────────┘
```

---

## How It Works (3 Steps)

### 1. **Detect** data type automatically

```
POST /v1/mrd/frame OR POST /v1/mrd/ingest
Body: [binary data]

Marshal checks:
├─ Has AcquisitionHeader? → Raw k-space
├─ Has ImageHeader?       → Reconstructed image
└─ Has HDF5 signature?    → Complete file
```

### 2. **Route** based on type

| Data Type | /v1/mrd/frame | /v1/mrd/ingest |
|-----------|---------------|----------------|
| **Raw k-space** | → Forward to recon service (future) | → Forward to recon service (future) |
| **Reconstructed** | → Store directly (existing) | → Allowed with warning |
| **HDF5 file** | → Forward to /ingest | → Save as-is (expected) |

### 3. **Store** final result

```
Raw k-space:  Scanner → Marshal → Recon Service → Marshal → Storage
Reconstructed: Scanner → Marshal → Storage (unchanged)
```

---

## What Changes?

### For Users

**Before:**
- Marshal rejects raw k-space ✗
- Must setup reconstruction pipeline separately

**After:**
- Marshal accepts raw k-space ✓
- Marshal accepts reconstructed ✓
- Automatic routing to reconstruction service

### For Existing Clients

**Nothing breaks!** Existing clients sending reconstructed images work exactly as before.

---

## Configuration

**Enable reconstruction routing:**

```bash
./mri_marshal \
  --data ./data \
  --recon-endpoint http://localhost:9002 \
  --recon-timeout 300
```

**Disable (backward compatible):**

```bash
./mri_marshal \
  --data ./data
# No --recon-endpoint = disabled
```

---

## Example: Sending Raw K-Space

```bash
# Send raw k-space line-by-line
curl -X POST http://localhost:8080/v1/mrd/frame \
  -H "X-MRD-Stream: cardiac_scan" \
  --data-binary @kspace_line_001.bin

# Response
{
  "status": "reconstruction_queued",
  "request_id": "uuid-1234",
  "acquisitions_buffered": 128
}

# Check status
curl http://localhost:8080/v1/mrd/reconstruction/status/uuid-1234

# Response
{
  "status": "processing",
  "progress": 0.65,
  "elapsed_sec": 28.5
}
```

---

## Key Components

| Component | Purpose | File |
|-----------|---------|------|
| **Type Detector** | Identify raw vs reconstructed | `src/mrd_type_detector.hpp` |
| **K-Space Buffer** | Accumulate acquisitions | `src/kspace_accumulator.hpp` |
| **Recon Client** | Talk to external service | `src/reconstruction_client.hpp` |
| **HTTP Handler** | Enhanced endpoint logic | `src/marshal_http.hpp` |

---

## Performance

| Metric | Value |
|--------|-------|
| Type detection | < 10 μs |
| Buffering overhead | < 50 μs |
| Memory per scan | ~32 MB |
| Reconstruction time | 30-90 seconds (service-dependent) |
| Existing path (unchanged) | < 1 ms |

---

## Timeline

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| 1. Detection logic | 1 week | Auto-detect raw vs reconstructed |
| 2. K-space buffer | 1 week | Accumulate acquisitions |
| 3. Recon client | 2 weeks | HTTP client + polling |
| 4. Integration | 2 weeks | Full end-to-end testing |

**Total:** 6 weeks

---

## Reconstruction Service API (Required)

External service must implement:

```
POST /reconstruct
  Body: k-space data (HDF5)
  Returns: request_id

GET /reconstruct/status/{id}
  Returns: {status, progress}

GET /reconstruct/result/{id}
  Returns: reconstructed images (ISMRMRD)
```

**Compatible with:**
- Gadgetron (with HTTP wrapper)
- Custom Python/Julia services
- Cloud services

---

## Questions?

**Q:** Does this break existing clients?
**A:** No! Backward compatible.

**Q:** What if reconstruction service is down?
**A:** Marshal saves k-space to disk, returns HTTP 503.

**Q:** Can I disable this feature?
**A:** Yes, omit `--recon-endpoint` flag.

**Q:** How much memory does it use?
**A:** ~320 MB for 10 buffered scans (configurable).

---

## References

- [ISMRMRD Documentation](https://ismrmrd.readthedocs.io/)
- [Full Design Doc](MRI_MARSHAL_RECONSTRUCTION_ROUTING.md) (detailed version)
- [GitHub Repository](https://github.com/cwru/mri-data-marshal)

---

**Summary:** Make MRI Marshal smart enough to handle raw k-space by auto-detecting data type and routing to external reconstruction service when needed.
