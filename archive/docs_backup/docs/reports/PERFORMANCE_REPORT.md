# CWRU Data Marshal: Performance & Latency Analysis

This document provides a comprehensive technical evaluation of the system's throughput, ingestion latency, and notification speed.

---

## 1. High-Level System Performance
These metrics represent the typical real-time performance seen by clinical users.

| Metric | Measured Value | Clinical Impact |
| :--- | :--- | :--- |
| **Peak Throughput** | **43.15 MB/s** | Can ingest high-res 3D volumes at ~10 FPS. |
| **Notification Delay** | **~25 ms** | Latency from Marshal to Visualizer via WebSocket. |
| **Total End-to-End** | **75ms - 160ms** | Well below the **<200ms** real-time threshold. |
| **Concurrency** | **100+ Requests** | Handled simultaneous Bio, Pose, and Bulk traffic. |

---

## 2. Ingestion Latency (SWMR vs. Bulk)
This section compares the time it takes for a scanner to "commit" a frame. 

**Key Finding:** SWMR Frame Streaming is significantly faster for data larger than 1MB because it avoids the overhead of creating new files and forced OS `fsync` calls.

| Resolution | Slices | Payload | SWMR Latency | Bulk Latency |
| :--- | :--- | :--- | :--- | :--- |
| **128x128** | 1 | 16 KB | 31 ms | 36 ms |
| **128x128** | 20 | 1.28 MB | **120 ms** | **1065 ms** |
| **192x192** | 10 | 1.44 MB | **71 ms** | **1105 ms** |
| **256x256** | 5 | 1.28 MB | **52 ms** | **1052 ms** |
| **256x256** | 20 | 5.12 MB | **136 ms** | **1098 ms** |

---

## 3. Scaling Grid (Resolution vs. Slices)
Detailed performance across a variety of clinical geometries (using SWMR).

| Res | Slices | Payload | Avg FPS | Avg MB/s |
| :--- | :--- | :--- | :--- | :--- |
| **64x64** | 20 | 320 KB | 35.38 | 10.61 |
| **128x128** | 20 | 1.28 MB | 26.67 | 32.89 |
| **256x256** | 20 | 5.12 MB | 8.25 | 41.27 |
| **512x512** | 5 | 5.12 MB | 8.54 | 42.70 |
| **512x512** | 20 | 20.48 MB | 0.98 | 19.53 |

---

## 4. Conclusion
The **HDF5 SWMR architecture** is the optimal choice for this system. It maintains sub-150ms latency for nearly all real-time geometries while providing a sustained throughput of ~40 MB/s for large volumes. The "Bulk Ingest" method should be reserved for background archival or post-processing tasks due to its 1-second commit overhead.

## Verification Tools
To reproduce these numbers, run the following:
*   `./scripts/benchmarks/benchmark_mrd.sh` (Throughput scaling)
*   `./scripts/benchmarks/latency_benchmark.sh` (SWMR vs Bulk comparison)
*   `./scripts/benchmarks/notification_latency.sh` (WebSocket speed)