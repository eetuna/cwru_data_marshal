# MRI Marshal System Diagram - Complete HTTP Flow

```
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│                                    MRI SCANNER / CLIENT                                  │
└────────────────┬────────────────────────────────────────────────────────────────────────┘
                 │
                 │ POST /v1/mrd/frame OR POST /v1/mrd/ingest
                 │ Body: Binary data (raw k-space, reconstructed image, or HDF5)
                 │
                 ▼
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│                           MRI MARSHAL :8080 (HTTP) :8090 (WS)                           │
│                                                                                          │
│  ┌────────────────────────────────────────────────────────────────────────────────────┐ │
│  │                           STEP 1: AUTO-DETECT DATA TYPE                            │ │
│  │                                                                                    │ │
│  │  Inspect binary headers:                                                          │ │
│  │  • HDF5 signature (8 bytes)?     → HDF5_FILE                                      │ │
│  │  • AcquisitionHeader (340 bytes)? → ACQUISITION (raw k-space)                     │ │
│  │  • ImageHeader (198 bytes)?       → IMAGE (reconstructed)                         │ │
│  │  • None match?                    → UNKNOWN                                       │ │
│  └────────────────┬───────────────────────────────────────────────────────────────────┘ │
│                   │                                                                      │
│  ┌────────────────┴───────────────────────────────────────────────────────────────────┐ │
│  │                           STEP 2: ROUTE BASED ON TYPE                              │ │
│  └────────┬─────────────────┬────────────────────┬───────────────────┬────────────────┘ │
│           │                 │                    │                   │                   │
│       HDF5_FILE         IMAGE              ACQUISITION           UNKNOWN                │
│           │                 │                    │                   │                   │
│           ▼                 ▼                    ▼                   ▼                   │
│  ┌─────────────┐  ┌──────────────────┐  ┌──────────────┐  ┌──────────────────┐        │
│  │ /frame:     │  │ Store to SWMR    │  │ Check recon  │  │ HTTP 400         │        │
│  │ → /ingest   │  │ (append frames)  │  │ configured?  │  │ Bad Request      │        │
│  │             │  │                  │  │              │  │ Invalid format   │        │
│  │ /ingest:    │  │ /frame: Normal   │  │ If NO:       │  └──────────────────┘        │
│  │ → Save file │  │ /ingest: Warning │  │ → HTTP 501   │                               │
│  └─────────────┘  └──────────────────┘  └──────┬───────┘                               │
│                                                 │                                        │
│                                                 │ If YES (--recon-endpoint set)          │
│                                                 ▼                                        │
│                                    ┌─────────────────────────┐                          │
│                                    │ STEP 3: FORWARD TO      │                          │
│                                    │ RECONSTRUCTION SERVICE  │                          │
│                                    └────────────┬────────────┘                          │
└─────────────────────────────────────────────────┼───────────────────────────────────────┘
                                                  │
                                                  │ POST /reconstruct
                                                  │ Body: Raw k-space (as-is)
                                                  │
                                                  ▼
                           ┌──────────────────────────────────────────┐
                           │   RECONSTRUCTION SERVICE :9002           │
                           │   (External - Gadgetron or custom)       │
                           │                                          │
                           │   • Receives: Raw k-space                │
                           │   • Processes: 2D/3D FFT reconstruction  │
                           │   • Returns: ImageHeader + pixels        │
                           └────────────────┬─────────────────────────┘
                                            │
                                            │ HTTP 200 OK
                                            │ Body: ImageHeader (198B) + pixels
                                            │
                                            ▼
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│                                    MRI MARSHAL                                           │
│                                                                                          │
│  ┌────────────────────────────────────────────────────────────────────────────────────┐ │
│  │                   STEP 4: RECEIVE & PARSE RECONSTRUCTED RESPONSE                   │ │
│  │                                                                                    │ │
│  │  • Read ImageHeader (first 198 bytes)                                             │ │
│  │  • Validate: matrix_size, channels, data_type                                     │ │
│  │  • Extract pixel data (remaining bytes)                                           │ │
│  └────────────────┬───────────────────────────────────────────────────────────────────┘ │
│                   │                                                                      │
│  ┌────────────────┴───────────────────────────────────────────────────────────────────┐ │
│  │                           STEP 5: STORE RECONSTRUCTED DATA                         │ │
│  │                                                                                    │ │
│  │  If original endpoint was /v1/mrd/frame:                                          │ │
│  │    → APPEND TO SWMR (streaming)                                                   │ │
│  │    → File: cardiac_scan.mrd (frames 0, 1, 2...)                                   │ │
│  │    → Viz clients can read while writing                                           │ │
│  │                                                                                    │ │
│  │  If original endpoint was /v1/mrd/ingest:                                         │ │
│  │    → SAVE COMPLETE FILE (batch)                                                   │ │
│  │    → File: cardiac_scan_reconstructed.mrd                                         │ │
│  │    → One request = One complete file                                              │ │
│  └────────────────┬───────────────────────────────────────────────────────────────────┘ │
│                   │                                                                      │
│                   ▼                                                                      │
│  ┌─────────────────────────────────────────────────────────────────────────────────┐   │
│  │                              STORAGE                                             │   │
│  │                                                                                  │   │
│  │  /session-data/run_YYYYMMDD_HHMMSS/mrd/                                         │   │
│  │  ├─ cardiac_scan.mrd          (SWMR - streaming frames)                         │   │
│  │  ├─ batch_complete.mrd         (Complete HDF5 file)                             │   │
│  │  ├─ bio.jsonl                  (ECG/biological signals)                         │   │
│  │  └─ poses.jsonl                (Pose tracking data)                             │   │
│  └─────────────────────────┬────────────────────────────────────────────────────────┘   │
└────────────────────────────┼─────────────────────────────────────────────────────────────┘
                             │
                             │ GET /v1/mrd/latest (HTTP)
                             │ GET /v1/mrd/since?last=N (HTTP)
                             │ Or direct HDF5 SWMR read
                             │
                             ▼
                    ┌─────────────────────┐
                    │   VIZ CLIENTS       │
                    │   (Visualization)   │
                    │                     │
                    │   • Read SWMR files │
                    │   • Display images  │
                    │   • Real-time view  │
                    └─────────────────────┘


═══════════════════════════════════════════════════════════════════════════════════════════
                                 HTTP ENDPOINTS SUMMARY
═══════════════════════════════════════════════════════════════════════════════════════════

┌──────────────────────┬─────────────────┬──────────────────────────────────────────────────┐
│ ENDPOINT             │ METHOD          │ DESCRIPTION                                      │
├──────────────────────┼─────────────────┼──────────────────────────────────────────────────┤
│ /v1/mrd/frame        │ POST            │ Streaming ingestion (frame-by-frame)            │
│                      │                 │ • Accepts: IMAGE (→SWMR), ACQUISITION (→recon), │
│                      │                 │   HDF5 (→internal forward to ingest logic)       │
├──────────────────────┼─────────────────┼──────────────────────────────────────────────────┤
│ /v1/mrd/ingest       │ POST            │ Batch ingestion (complete files)                │
│                      │                 │ • Accepts: HDF5 (→save file), IMAGE (→SWMR),    │
│                      │                 │   ACQUISITION (→recon→save file)                 │
├──────────────────────┼─────────────────┼──────────────────────────────────────────────────┤
│ /v1/mrd/latest       │ GET             │ Get latest frame metadata                       │
├──────────────────────┼─────────────────┼──────────────────────────────────────────────────┤
│ /v1/mrd/since        │ GET             │ Get frames since timestamp or last N            │
├──────────────────────┼─────────────────┼──────────────────────────────────────────────────┤
│ /v1/bio/signal       │ POST            │ Submit biological signal (ECG)                  │
├──────────────────────┼─────────────────┼──────────────────────────────────────────────────┤
│ /v1/bio/latest       │ GET             │ Get latest biological signal                    │
├──────────────────────┼─────────────────┼──────────────────────────────────────────────────┤
│ /v1/pose/update      │ POST            │ Submit pose/tracking data                       │
├──────────────────────┼─────────────────┼──────────────────────────────────────────────────┤
│ /v1/pose/current     │ GET             │ Get current pose                                │
├──────────────────────┼─────────────────┼──────────────────────────────────────────────────┤
│ /health              │ GET             │ Health check                                    │
└──────────────────────┴─────────────────┴──────────────────────────────────────────────────┘


═══════════════════════════════════════════════════════════════════════════════════════════
                              DATA TYPE DETECTION RULES
═══════════════════════════════════════════════════════════════════════════════════════════

┌──────────────────┬─────────────────────────────────┬──────────────────────────────────┐
│ DATA TYPE        │ DETECTION METHOD                │ ACTION                           │
├──────────────────┼─────────────────────────────────┼──────────────────────────────────┤
│ HDF5_FILE        │ First 8 bytes:                  │ /frame → Forward to ingest logic │
│                  │ 89 48 44 46 0D 0A 1A 0A         │ /ingest → Save complete file     │
├──────────────────┼─────────────────────────────────┼──────────────────────────────────┤
│ IMAGE            │ ImageHeader (198 bytes):        │ Store to SWMR                    │
│                  │ • version = 1                   │ (append frame-by-frame)          │
│                  │ • matrix_size = 1-4096          │                                  │
│                  │ • channels = 1-128              │                                  │
│                  │ • data_type = 1-10              │                                  │
├──────────────────┼─────────────────────────────────┼──────────────────────────────────┤
│ ACQUISITION      │ AcquisitionHeader (340 bytes):  │ If recon configured:             │
│ (raw k-space)    │ • version = 1                   │   → Forward to recon service     │
│                  │ • number_of_samples = 1-32768   │   → Store reconstructed result   │
│                  │ • active_channels = 1-128       │ If NOT configured:               │
│                  │ • trajectory_dimensions = 0-3   │   → HTTP 501                     │
├──────────────────┼─────────────────────────────────┼──────────────────────────────────┤
│ UNKNOWN          │ No valid signature detected     │ HTTP 400 Bad Request             │
└──────────────────┴─────────────────────────────────┴──────────────────────────────────┘


═══════════════════════════════════════════════════════════════════════════════════════════
                            STORAGE DECISION AFTER RECONSTRUCTION
═══════════════════════════════════════════════════════════════════════════════════════════

  Original Endpoint               Storage Method              Result
  ─────────────────               ──────────────              ──────

  POST /v1/mrd/frame       →      SWMR (append)        →     cardiac_scan.mrd
  (streaming)                     Frame 0, 1, 2...            (one growing file)
                                  Real-time readable


  POST /v1/mrd/ingest      →      Complete file        →     cardiac_scan_001.mrd
  (batch)                         Write entire file           cardiac_scan_002.mrd
                                  Close immediately           (separate files)


═══════════════════════════════════════════════════════════════════════════════════════════
                                  KEY CONFIGURATION
═══════════════════════════════════════════════════════════════════════════════════════════

  WITHOUT reconstruction:
  ──────────────────────
  ./marshal --http 0.0.0.0:8080 --ws 0.0.0.0:8090 --data /session-data

  → Raw k-space (ACQUISITION): HTTP 501 "Not configured"
  → Reconstructed images (IMAGE): Store directly to SWMR


  WITH reconstruction:
  ───────────────────
  ./marshal --http 0.0.0.0:8080 --ws 0.0.0.0:8090 --data /session-data \
            --recon-endpoint http://reconstruction-service:9002

  → Raw k-space (ACQUISITION): Forward → Recon → Store
  → Reconstructed images (IMAGE): Store directly to SWMR (unchanged)


═══════════════════════════════════════════════════════════════════════════════════════════
```
