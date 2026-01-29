# CWRU Data Marshal - Traditional Flowcharts

**Generated:** 2026-01-27

This document contains traditional flowchart representations of the marshal architecture, data flows, and use cases.

---

## Table of Contents

1. [MRI Marshal Request Flow](#1-mri-marshal-request-flow)
2. [Robot Marshal Request Flow](#2-robot-marshal-request-flow)
3. [Image Streaming Use Case](#3-image-streaming-use-case)
4. [ECG Signal Processing](#4-ecg-signal-processing)
5. [Catheter Control Loop](#5-catheter-control-loop)
6. [WebSocket Subscription Flow](#6-websocket-subscription-flow)
7. [HDF5 SWMR Read/Write Flow](#7-hdf5-swmr-readwrite-flow)
8. [System Startup Sequence](#8-system-startup-sequence)

---

## 1. MRI Marshal Request Flow

### POST /v1/mrd/frame - Frame Ingestion Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                         START                                    │
│                  (Client POST Request)                           │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ HTTP Listener    │
                   │ (Port 8080)      │
                   │ Accepts Connection│
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Parse HTTP       │
                   │ Headers & Body   │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Extract Stream   │
                   │ ID from Header   │
                   │ (X-MRD-Stream)   │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Route to         │
                   │ POST /v1/mrd/    │
                   │ frame handler    │
                   └─────────┬────────┘
                             │
                             ▼
                 ┌───────────────────────┐
                 │ Validate ISMRMRD      │
                 │ Binary Format         │
                 └─────────┬─────────────┘
                           │
                ┌──────────┴──────────┐
                │                     │
                ▼                     ▼
         ┌───────────┐         ┌───────────┐
         │  Valid?   │         │  Invalid? │
         └─────┬─────┘         └─────┬─────┘
               │ YES                 │ NO
               │                     ▼
               │           ┌──────────────────┐
               │           │ Return HTTP 400  │
               │           │ Bad Request      │
               │           └────────┬─────────┘
               │                    │
               │                    ▼
               │              ┌──────────┐
               │              │   END    │
               │              └──────────┘
               │
               ▼
    ┌────────────────────────┐
    │ Get/Create MrdSink     │
    │ for Stream ID          │
    └──────────┬─────────────┘
               │
               ▼
    ┌────────────────────────┐
    │ MrdSink::append_frame()│
    │ (Thread-safe)          │
    └──────────┬─────────────┘
               │
               │ ┌─────────────────────────────────┐
               │ │   PARALLEL OPERATIONS           │
               │ └─────────────────────────────────┘
               │
    ┏━━━━━━━━━┻━━━━━━━━━┓
    ┃                    ┃
    ▼                    ▼
┌─────────────┐    ┌──────────────┐
│ Write to    │    │ Update In-   │
│ HDF5 File   │    │ Memory Cache │
│ (SWMR mode) │    │ (latest.*)   │
└──────┬──────┘    └──────┬───────┘
       │                  │
       ▼                  ▼
┌─────────────┐    ┌──────────────┐
│ Flush to    │    │ Queue JSON   │
│ Disk (async)│    │ Write        │
└──────┬──────┘    └──────┬───────┘
       │                  │
       ┗━━━━━━┳━━━━━━━━━━┛
              │
              ▼
    ┌──────────────────┐
    │ Notify WebSocket │
    │ Subscribers      │
    │ (topic: "mrd")   │
    └─────────┬────────┘
              │
              ▼
    ┌──────────────────┐
    │ Generate         │
    │ Frame Index      │
    └─────────┬────────┘
              │
              ▼
    ┌──────────────────┐
    │ Return HTTP 200  │
    │ {frame_index: N} │
    └─────────┬────────┘
              │
              ▼
        ┌──────────┐
        │   END    │
        └──────────┘
```

### GET /v1/mrd/latest - Get Latest Frame Metadata

```
┌─────────────────────────────────────────────────────────────────┐
│                         START                                    │
│                  (Client GET Request)                            │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ HTTP Listener    │
                   │ Receives GET     │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Parse Query      │
                   │ Parameters       │
                   │ ?stream=...      │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Route to GET     │
                   │ /v1/mrd/latest   │
                   │ handler          │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Lock Shared      │
                   │ State Mutex      │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Read from        │
                   │ In-Memory Cache  │
                   │ (latest_mrd_)    │
                   └─────────┬────────┘
                             │
                             ▼
                  ┌────────────────────┐
                  │ Cache Has Data?    │
                  └─────────┬──────────┘
                            │
                ┌───────────┴───────────┐
                │                       │
                ▼                       ▼
         ┌────────────┐         ┌────────────┐
         │    YES     │         │     NO     │
         └──────┬─────┘         └──────┬─────┘
                │                      │
                ▼                      ▼
      ┌──────────────────┐   ┌──────────────────┐
      │ Format Response: │   │ Return HTTP 404  │
      │ {                │   │ Not Found        │
      │   path: "...",   │   └────────┬─────────┘
      │   frame_index: N │            │
      │ }                │            ▼
      └────────┬─────────┘      ┌──────────┐
               │                │   END    │
               ▼                └──────────┘
      ┌──────────────────┐
      │ Unlock Mutex     │
      └────────┬─────────┘
               │
               ▼
      ┌──────────────────┐
      │ Return HTTP 200  │
      │ with JSON        │
      └────────┬─────────┘
               │
               ▼
         ┌──────────┐
         │   END    │
         └──────────┘
```

---

## 2. Robot Marshal Request Flow

### POST /write/<filename> - Write to Data Channel

```
┌─────────────────────────────────────────────────────────────────┐
│                         START                                    │
│            (Client POST /write/localization_data)                │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ HTTP Server      │
                   │ (cpp-httplib)    │
                   │ Thread Pool      │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Extract Filename │
                   │ from URL Path    │
                   │ "localization_   │
                   │  data"           │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Parse JSON Body: │
                   │ {                │
                   │   sent_at: ...,  │
                   │   values: [...]  │
                   │ }                │
                   └─────────┬────────┘
                             │
                             ▼
                  ┌────────────────────┐
                  │ Filename Valid?    │
                  │ (exists in config) │
                  └─────────┬──────────┘
                            │
                ┌───────────┴───────────┐
                │                       │
                ▼                       ▼
         ┌────────────┐         ┌────────────┐
         │    YES     │         │     NO     │
         └──────┬─────┘         └──────┬─────┘
                │                      │
                │                      ▼
                │            ┌──────────────────┐
                │            │ Return HTTP 404  │
                │            │ File Not Found   │
                │            └────────┬─────────┘
                │                     │
                │                     ▼
                │               ┌──────────┐
                │               │   END    │
                │               └──────────┘
                │
                ▼
      ┌──────────────────────┐
      │ Acquire Exclusive    │
      │ Lock on File Mutex   │
      │ (write lock)         │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Add Entry to         │
      │ CircularBuffer       │
      │ (in-memory)          │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Queue Entry for      │
      │ Background Disk      │
      │ Write (async)        │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Notify Writer        │
      │ Thread (condition    │
      │ variable)            │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Release File Mutex   │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Return HTTP 200 OK   │
      └──────────┬───────────┘
                 │
                 ▼
           ┌──────────┐
           │   END    │
           └──────────┘

┌─────────────────────────────────────────────────┐
│        BACKGROUND WRITER THREAD                 │
│        (runs independently)                     │
└──────────────────┬──────────────────────────────┘
                   │
                   ▼
         ┌──────────────────┐
         │ Wait on          │
         │ Condition        │
         │ Variable         │
         └────────┬─────────┘
                  │
                  ▼
         ┌──────────────────┐
         │ Wake Up on       │
         │ New Write        │
         └────────┬─────────┘
                  │
                  ▼
         ┌──────────────────┐
         │ Dequeue Entry    │
         │ from Write Queue │
         └────────┬─────────┘
                  │
                  ▼
         ┌──────────────────┐
         │ Write JSON to    │
         │ /files/<name>.   │
         │ json             │
         └────────┬─────────┘
                  │
                  ▼
         ┌──────────────────┐
         │ Flush to Disk    │
         └────────┬─────────┘
                  │
                  ▼
         ┌──────────────────┐
         │ Return to Wait   │
         │ State            │
         └──────────────────┘
```

### GET /read/<filename> - Read from Data Channel

```
┌─────────────────────────────────────────────────────────────────┐
│                         START                                    │
│             (Client GET /read/tip_position_orientation)          │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ HTTP Server      │
                   │ Thread Pool      │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Extract Filename │
                   │ from URL         │
                   └─────────┬────────┘
                             │
                             ▼
                  ┌────────────────────┐
                  │ Filename Valid?    │
                  └─────────┬──────────┘
                            │
                ┌───────────┴───────────┐
                │                       │
                ▼                       ▼
         ┌────────────┐         ┌────────────┐
         │    YES     │         │     NO     │
         └──────┬─────┘         └──────┬─────┘
                │                      │
                │                      ▼
                │            ┌──────────────────┐
                │            │ Return HTTP 404  │
                │            └────────┬─────────┘
                │                     │
                │                     ▼
                │               ┌──────────┐
                │               │   END    │
                │               └──────────┘
                │
                ▼
      ┌──────────────────────┐
      │ Acquire Shared Lock  │
      │ on File Mutex        │
      │ (read lock)          │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Read All Entries     │
      │ from CircularBuffer  │
      │ (up to 1000)         │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Format as JSON:      │
      │ {                    │
      │   entries: [         │
      │     {sent_at, values}│
      │     ...              │
      │   ]                  │
      │ }                    │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Release Shared Lock  │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Return HTTP 200      │
      │ with JSON payload    │
      └──────────┬───────────┘
                 │
                 ▼
           ┌──────────┐
           │   END    │
           └──────────┘
```

---

## 3. Image Streaming Use Case

### Complete Flow: Image Streamer → Visualization

```
┌───────────────────────────────────────────────────────────────────┐
│                          START                                     │
│                   (Image Streamer Boot)                            │
└────────────────────────────┬──────────────────────────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Initialize       │
                   │ ISMRMRD          │
                   │ Generator        │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Set Frame Rate   │
                   │ (e.g., 20 FPS)   │
                   └─────────┬────────┘
                             │
           ┌─────────────────┴─────────────────┐
           │         MAIN LOOP                 │
           │  (runs at configured FPS)         │
           └─────────────────┬─────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Generate Synthetic│
                   │ MRI Frame         │
                   │ (64x64x3 complex) │
                   └─────────┬─────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Encode as        │
                   │ ISMRMRD Binary   │
                   │ (~64 KB)         │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ HTTP POST to     │
                   │ /v1/mrd/frame    │
                   │ (MRI Marshal)    │
                   └─────────┬────────┘
                             │
                             ▼
              ┏━━━━━━━━━━━━━━━━━━━━━━━━┓
              ┃   MRI MARSHAL PROCESS   ┃
              ┗━━━━━━━━━┳━━━━━━━━━━━━━━┛
                        │
                        ▼
              ┌──────────────────┐
              │ Receive Frame    │
              │ (marshal_http)   │
              └─────────┬────────┘
                        │
                        ▼
              ┌──────────────────┐
              │ MrdSink::        │
              │ append_frame()   │
              └─────────┬────────┘
                        │
          ┏━━━━━━━━━━━┻━━━━━━━━━━━┓
          ┃                        ┃
          ▼                        ▼
    ┌──────────┐          ┌──────────────┐
    │ Write to │          │ Update Cache │
    │ HDF5     │          │ (latest_mrd_)│
    │ SWMR     │          └──────┬───────┘
    └────┬─────┘                 │
         │                       │
         ▼                       ▼
    ┌──────────┐          ┌──────────────┐
    │ Flush    │          │ Broadcast    │
    │ (async)  │          │ WebSocket    │
    └──────────┘          │ {type:"mrd"} │
                          └──────┬───────┘
                                 │
                                 ▼
              ┏━━━━━━━━━━━━━━━━━━━━━━━━┓
              ┃   VISUALIZATION CLIENT  ┃
              ┗━━━━━━━━━┳━━━━━━━━━━━━━━┛
                        │
                        ▼
              ┌──────────────────┐
              │ Receive WS       │
              │ Notification     │
              │ on topic "mrd"   │
              └─────────┬────────┘
                        │
                        ▼
              ┌──────────────────┐
              │ Extract          │
              │ frame_index      │
              │ from message     │
              └─────────┬────────┘
                        │
                        ▼
              ┌──────────────────┐
              │ HTTP GET         │
              │ /v1/mrd/latest   │
              └─────────┬────────┘
                        │
                        ▼
              ┌──────────────────┐
              │ Get HDF5 File    │
              │ Path & Index     │
              └─────────┬────────┘
                        │
                        ▼
              ┌──────────────────┐
              │ Open HDF5 File   │
              │ in SWMR Read     │
              │ Mode             │
              └─────────┬────────┘
                        │
                        ▼
              ┌──────────────────┐
              │ Call dset.       │
              │ refresh()        │
              │ (see new writes) │
              └─────────┬────────┘
                        │
                        ▼
              ┌──────────────────┐
              │ Read Frame Data  │
              │ [index][0][z][y] │
              │ [x]              │
              └─────────┬────────┘
                        │
                        ▼
              ┌──────────────────┐
              │ Convert to       │
              │ OpenCV Mat       │
              └─────────┬────────┘
                        │
                        ▼
              ┌──────────────────┐
              │ Display 3 Slices │
              │ in Windows       │
              └─────────┬────────┘
                        │
                        ▼
              ┌──────────────────┐
              │ Wait for Next    │
              │ WS Notification  │
              └─────────┬────────┘
                        │
                        │
           ─────────────┴─────────────
           Loop back to "Receive WS"
```

---

## 4. ECG Signal Processing

### ECG Client → Bio Signal Storage → Consumer

```
┌─────────────────────────────────────────────────────────────────┐
│                         START                                    │
│                   (ECG Client Boot)                              │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Initialize ECG   │
                   │ Simulator        │
                   │ (BPM: 72)        │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Set Sampling     │
                   │ Rate: 250 Hz     │
                   └─────────┬────────┘
                             │
           ┌─────────────────┴─────────────────┐
           │      CONTINUOUS LOOP              │
           │   (every 4ms = 250 Hz)            │
           └─────────────────┬─────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Generate ECG     │
                   │ Sample           │
                   │ (P-QRS-T wave)   │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Accumulate       │
                   │ Samples in       │
                   │ Buffer (1 sec)   │
                   └─────────┬────────┘
                             │
                             ▼
                  ┌────────────────────┐
                  │ Buffer Full?       │
                  │ (250 samples)      │
                  └─────────┬──────────┘
                            │
                ┌───────────┴───────────┐
                │                       │
                ▼                       ▼
         ┌────────────┐         ┌────────────┐
         │    YES     │         │     NO     │
         └──────┬─────┘         └──────┬─────┘
                │                      │
                │                      ▼
                │            ┌──────────────────┐
                │            │ Continue Loop    │
                │            └──────────────────┘
                │
                ▼
      ┌──────────────────────┐
      │ Format JSON Payload: │
      │ {                    │
      │   source: "ecg_1",   │
      │   data: [250 vals],  │
      │   rate_hz: 250.0     │
      │ }                    │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ HTTP POST to         │
      │ /v1/bio/signal       │
      │ (MRI Marshal)        │
      └──────────┬───────────┘
                 │
                 ▼
       ┏━━━━━━━━━━━━━━━━━━━━┓
       ┃   MRI MARSHAL       ┃
       ┗━━━━━━━━┳━━━━━━━━━━━┛
                │
                ▼
      ┌──────────────────────┐
      │ Route to             │
      │ POST /v1/bio/signal  │
      │ Handler              │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Lock bio_cache_      │
      │ Mutex                │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Update In-Memory     │
      │ latest_bio_ Cache    │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Queue JSON Write     │
      │ to bio.jsonl         │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Broadcast to WS      │
      │ Subscribers          │
      │ (topic: "bio")       │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Unlock Mutex         │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Return HTTP 200      │
      └──────────┬───────────┘
                 │
                 ▼
       ┏━━━━━━━━━━━━━━━━━━━━┓
       ┃   ECG CONSUMER      ┃
       ┃  (e.g., Monitor)    ┃
       ┗━━━━━━━━┳━━━━━━━━━━━┛
                │
                ▼
      ┌──────────────────────┐
      │ Receive WS Message   │
      │ {type: "bio"}        │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ HTTP GET             │
      │ /v1/bio/latest       │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Get ECG Data         │
      │ (250 samples)        │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Display ECG          │
      │ Waveform on          │
      │ Monitor              │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Wait for Next        │
      │ Notification         │
      └──────────────────────┘
```

---

## 5. Catheter Control Loop

### 50-80 Hz Control Cycle (All 5 Clients)

```
┌─────────────────────────────────────────────────────────────────┐
│                    ITERATION N START                             │
│                (Cycle Time: ~12-20 ms)                           │
└────────────────────────────┬────────────────────────────────────┘
                             │
           ┌─────────────────┴─────────────────┐
           │                                   │
           │  ALL CLIENTS RUN IN PARALLEL      │
           │  (via Robot Marshal blackboard)   │
           │                                   │
           └─────────────────┬─────────────────┘
                             │
      ┏━━━━━━━━━━━━━━━━━━━━━┻━━━━━━━━━━━━━━━━━━━━━┓
      ┃                                            ┃
      ▼                                            ▼
┌──────────────────┐                    ┌──────────────────┐
│ CATHETER TRACKING│                    │   FRONT-END      │
│     CLIENT       │                    │     CLIENT       │
└────────┬─────────┘                    └────────┬─────────┘
         │                                       │
         ▼                                       ▼
   ┌──────────┐                           ┌──────────┐
   │ GET /read│                           │ GET /read│
   │ /localiz-│                           │ /streamin│
   │ ation_   │                           │ g_2D_    │
   │ data     │                           │ images   │
   └────┬─────┘                           └────┬─────┘
        │                                      │
        ▼                                      ▼
   ┌──────────┐                           ┌──────────┐
   │ Compute  │                           │ Render   │
   │ Forward  │                           │ UI with  │
   │ Kinematic│                           │ Images + │
   │ s        │                           │ Tip      │
   └────┬─────┘                           └────┬─────┘
        │                                      │
        ▼                                      ▼
   ┌──────────┐                           ┌──────────┐
   │ Calculate│                           │ Capture  │
   │ Tip      │                           │ Doctor   │
   │ Position │                           │ Input    │
   │ & Orient │                           │ (click)  │
   └────┬─────┘                           └────┬─────┘
        │                                      │
        ▼                                      ▼
   ┌──────────┐                           ┌──────────┐
   │ POST     │                           │ POST     │
   │ /write/  │                           │ /write/  │
   │ tip_pos- │                           │ user_    │
   │ ition_   │                           │ input    │
   │ orientat-│                           │          │
   │ ion      │                           │          │
   └──────────┘                           └──────────┘
        │                                      │
        │                                      │
        ▼                                      ▼
      ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
      ┃        ROBOT MARSHAL                   ┃
      ┃   (stores all data in blackboard)      ┃
      ┗━━━━━━━━━━━━━┳━━━━━━━━━━━━━━━━━━━━━━━━━┛
                    │
                    ▼
      ┌─────────────────────────────┐
      │ All Channels Updated:       │
      │ - tip_position_orientation  │
      │ - user_input                │
      │ - surface_model_parameters  │
      │ - biological_signals        │
      └─────────────┬───────────────┘
                    │
      ┏━━━━━━━━━━━━━┻━━━━━━━━━━━━━┓
      ┃                            ┃
      ▼                            ▼
┌──────────────┐           ┌──────────────┐
│  PLANNING    │           │ CONTROLLER   │
│   CLIENT     │           │   CLIENT     │
└──────┬───────┘           └──────┬───────┘
       │                          │
       ▼                          ▼
  ┌──────────┐              ┌──────────┐
  │ GET /read│              │ GET /read│
  │ /tip_pos-│              │ /desired_│
  │ ition    │              │ planned_ │
  └────┬─────┘              │ motion   │
       │                    └────┬─────┘
       ▼                         │
  ┌──────────┐                   ▼
  │ GET /read│              ┌──────────┐
  │ /surface_│              │ Compute  │
  │ model    │              │ Inverse  │
  └────┬─────┘              │ Kinematic│
       │                    │ s        │
       ▼                    └────┬─────┘
  ┌──────────┐                   │
  │ GET /read│                   ▼
  │ /user_   │              ┌──────────┐
  │ input    │              │ Generate │
  └────┬─────┘              │ Joint    │
       │                    │ Commands │
       ▼                    └────┬─────┘
  ┌──────────┐                   │
  │ Path     │                   ▼
  │ Planning │              ┌──────────┐
  │ Algorithm│              │ POST     │
  └────┬─────┘              │ /write/  │
       │                    │ forward_ │
       ▼                    │ kinematic│
  ┌──────────┐              │ s        │
  │ Generate │              └──────────┘
  │ Motion   │
  │ Plan     │
  └────┬─────┘
       │
       ▼
  ┌──────────┐
  │ POST     │
  │ /write/  │
  │ desired_ │
  │ planned_ │
  │ motion   │
  └──────────┘
       │
       │
       ▼
┌──────────────────┐
│ SURFACE TRACKING │
│     CLIENT       │
└────────┬─────────┘
         │
         ▼
   ┌──────────┐
   │ GET /read│
   │ /biologic│
   │ al_signal│
   │ s        │
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │ GET /read│
   │ /streamin│
   │ g_2D_    │
   │ images   │
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │ 3D       │
   │ Reconstru│
   │ ction    │
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │ Update   │
   │ Surface  │
   │ Model    │
   └────┬─────┘
        │
        ▼
   ┌──────────┐
   │ POST     │
   │ /write/  │
   │ surface_ │
   │ model_   │
   │ parameter│
   │ s        │
   └──────────┘
        │
        │
        ▼
┌─────────────────────────────────────────────────────────────────┐
│                  ITERATION N COMPLETE                            │
│              (All clients updated state)                         │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Sleep for        │
                   │ Remaining Time   │
                   │ to maintain      │
                   │ 50-80 Hz rate    │
                   └─────────┬────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                   ITERATION N+1 START                            │
│                  (Loop back to top)                              │
└─────────────────────────────────────────────────────────────────┘
```

---

## 6. WebSocket Subscription Flow

### Client Subscribing to MRI Frames

```
┌─────────────────────────────────────────────────────────────────┐
│                         START                                    │
│              (Client Application Launch)                         │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Create WebSocket │
                   │ Client           │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Connect to       │
                   │ ws://localhost:  │
                   │ 8090/ws          │
                   └─────────┬────────┘
                             │
                             ▼
                  ┌────────────────────┐
                  │ Connection         │
                  │ Successful?        │
                  └─────────┬──────────┘
                            │
                ┌───────────┴───────────┐
                │                       │
                ▼                       ▼
         ┌────────────┐         ┌────────────┐
         │    YES     │         │     NO     │
         └──────┬─────┘         └──────┬─────┘
                │                      │
                │                      ▼
                │            ┌──────────────────┐
                │            │ Retry Connection │
                │            │ (backoff)        │
                │            └────────┬─────────┘
                │                     │
                │                     ▼
                │               ┌──────────┐
                │               │   END    │
                │               └──────────┘
                │
                ▼
      ┌──────────────────────┐
      │ Receive WS           │
      │ Connection Event     │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Send Subscription:   │
      │ {                    │
      │   "subscribe": "mrd" │
      │ }                    │
      └──────────┬───────────┘
                 │
                 ▼
       ┏━━━━━━━━━━━━━━━━━━━━┓
       ┃   MRI MARSHAL       ┃
       ┃   WS Server         ┃
       ┗━━━━━━━━┳━━━━━━━━━━━┛
                │
                ▼
      ┌──────────────────────┐
      │ Parse Subscribe      │
      │ Message              │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Add Client to        │
      │ Topic Subscriber     │
      │ List (topic="mrd")   │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Client Now           │
      │ Subscribed           │
      └──────────┬───────────┘
                 │
                 │
           ┌─────┴─────────────────────┐
           │   WAIT FOR DATA EVENTS    │
           └─────┬─────────────────────┘
                 │
                 ▼
       ┏━━━━━━━━━━━━━━━━━━━━┓
       ┃   When MRI Frame    ┃
       ┃   is Posted         ┃
       ┗━━━━━━━━┳━━━━━━━━━━━┛
                │
                ▼
      ┌──────────────────────┐
      │ Marshal Broadcasts:  │
      │ {                    │
      │   type: "mrd",       │
      │   frame_index: 42,   │
      │   timestamp: "..."   │
      │ }                    │
      └──────────┬───────────┘
                 │
                 ▼
       ┏━━━━━━━━━━━━━━━━━━━━┓
       ┃   CLIENT RECEIVES   ┃
       ┗━━━━━━━━┳━━━━━━━━━━━┛
                │
                ▼
      ┌──────────────────────┐
      │ Trigger Callback     │
      │ on_message(msg)      │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Extract frame_index  │
      │ from message         │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Process Frame        │
      │ (e.g., fetch & show) │
      └──────────┬───────────┘
                 │
                 ▼
      ┌──────────────────────┐
      │ Wait for Next        │
      │ Message              │
      └──────────────────────┘
```

---

## 7. HDF5 SWMR Read/Write Flow

### Concurrent Writer and Multiple Readers

```
┌─────────────────────────────────────────────────────────────────┐
│                    SYSTEM INITIALIZATION                         │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ MRI Marshal      │
                   │ Creates HDF5     │
                   │ File             │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Open File in     │
                   │ SWMR Write Mode  │
                   │ (H5F_ACC_SWMR_   │
                   │  WRITE)          │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Create Dataset:  │
                   │ /images/data     │
                   │ [frames, ch, z,  │
                   │  y, x]           │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Set Unlimited    │
                   │ Max Dimensions   │
                   │ on frame axis    │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ File Ready for   │
                   │ SWMR Operations  │
                   └─────────┬────────┘
                             │
           ┌─────────────────┴─────────────────┐
           │                                   │
           ▼                                   ▼
    ┏━━━━━━━━━━━━━━┓              ┏━━━━━━━━━━━━━━━━━┓
    ┃  WRITER      ┃              ┃  READERS        ┃
    ┃  (Marshal)   ┃              ┃  (Clients 1-N)  ┃
    ┗━━━━━━┳━━━━━━━┛              ┗━━━━━━┳━━━━━━━━━━┛
           │                              │
           ▼                              ▼
  ┌──────────────┐              ┌─────────────────┐
  │ Receive New  │              │ Each Reader     │
  │ Frame from   │              │ Opens File in   │
  │ Image        │              │ SWMR Read Mode  │
  │ Streamer     │              │ (H5F_ACC_RDONLY │
  └──────┬───────┘              │ | H5F_ACC_SWMR_ │
         │                      │  READ)          │
         ▼                      └────────┬────────┘
  ┌──────────────┐                      │
  │ Lock HDF5    │                      ▼
  │ Write Mutex  │              ┌─────────────────┐
  └──────┬───────┘              │ Open Dataset    │
         │                      │ /images/data    │
         ▼                      └────────┬────────┘
  ┌──────────────┐                      │
  │ Extend       │                      ▼
  │ Dataset      │              ┌─────────────────┐
  │ Dimensions   │              │ Get Current     │
  │ (+1 frame)   │              │ Dimensions      │
  └──────┬───────┘              └────────┬────────┘
         │                               │
         ▼                               ▼
  ┌──────────────┐                      │
  │ Select       │              ┌───────┴────────┐
  │ Hyperslab    │              │                │
  │ for New      │              ▼                ▼
  │ Frame        │     ┌─────────────┐  ┌─────────────┐
  └──────┬───────┘     │  READER 1   │  │  READER 2   │
         │             └──────┬──────┘  └──────┬──────┘
         ▼                    │                │
  ┌──────────────┐            ▼                ▼
  │ Write Frame  │   ┌─────────────┐  ┌─────────────┐
  │ Data         │   │ Call dset.  │  │ Call dset.  │
  │ (64x64x3)    │   │ refresh()   │  │ refresh()   │
  └──────┬───────┘   └──────┬──────┘  └──────┬──────┘
         │                  │                │
         ▼                  ▼                ▼
  ┌──────────────┐   ┌─────────────┐  ┌─────────────┐
  │ Flush        │   │ See Latest  │  │ See Latest  │
  │ Dataset      │   │ Dimensions  │  │ Dimensions  │
  │ to Disk      │   │ (updated)   │  │ (updated)   │
  └──────┬───────┘   └──────┬──────┘  └──────┬──────┘
         │                  │                │
         ▼                  ▼                ▼
  ┌──────────────┐   ┌─────────────┐  ┌─────────────┐
  │ Unlock HDF5  │   │ Read Frame  │  │ Read Frame  │
  │ Mutex        │   │ [index][0]  │  │ [index][0]  │
  └──────┬───────┘   │ [z][y][x]   │  │ [z][y][x]   │
         │           └──────┬──────┘  └──────┬──────┘
         ▼                  │                │
  ┌──────────────┐          ▼                ▼
  │ Frame Index  │   ┌─────────────┐  ┌─────────────┐
  │ Incremented  │   │ Process     │  │ Process     │
  └──────┬───────┘   │ Frame Data  │  │ Frame Data  │
         │           └─────────────┘  └─────────────┘
         │
         │  ┌────────────────────────────┐
         │  │  Multiple Readers Can      │
         │  │  Simultaneously Read       │
         │  │  Without Blocking          │
         │  └────────────────────────────┘
         │
         │
         └──────── Loop back to "Receive New Frame"
```

### Key SWMR Characteristics

```
┌─────────────────────────────────────────────────────────────────┐
│                   SWMR GUARANTEES                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ✓ ONE Writer + MULTIPLE Readers Concurrently                   │
│  ✓ Readers See Consistent Snapshots                             │
│  ✓ Writers Must Flush After Each Write                          │
│  ✓ Readers Must refresh() to See New Data                       │
│  ✓ No File Corruption (atomic operations)                       │
│  ✓ Readers Can Join/Leave Anytime                               │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                   SWMR RESTRICTIONS                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ✗ Cannot Add/Remove Datasets After SWMR Start                  │
│  ✗ Cannot Change Attributes After SWMR Start                    │
│  ✗ Can Only Extend Unlimited Dimensions                         │
│  ✗ Must Use Chunked Storage Layout                              │
│  ✗ Requires HDF5 1.10+                                          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 8. System Startup Sequence

### Docker Compose Boot Process

```
┌─────────────────────────────────────────────────────────────────┐
│                         START                                    │
│          (docker-compose -f demo.yml up)                         │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Load .env.demo   │
                   │ Environment Vars │
                   └─────────┬────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ Create Networks  │
                   │ and Volumes      │
                   └─────────┬────────┘
                             │
                             ▼
           ┌─────────────────┴─────────────────┐
           │   START ALL SERVICES IN PARALLEL  │
           └─────────────────┬─────────────────┘
                             │
      ┏━━━━━━━━━━━━━━━━━━━━━┻━━━━━━━━━━━━━━━━━━━━━┓
      ┃                                            ┃
      ▼                                            ▼
┌──────────────────┐                    ┌──────────────────┐
│  MRI MARSHAL     │                    │  ROBOT MARSHAL   │
│  CONTAINER       │                    │  CONTAINER       │
└────────┬─────────┘                    └────────┬─────────┘
         │                                       │
         ▼                                       ▼
   ┌──────────┐                           ┌──────────┐
   │ Build    │                           │ Build    │
   │ C++ Code │                           │ C++ Code │
   └────┬─────┘                           └────┬─────┘
        │                                      │
        ▼                                      ▼
   ┌──────────┐                           ┌──────────┐
   │ Start    │                           │ Start    │
   │ mri_     │                           │ robot_   │
   │ marshal  │                           │ marshal  │
   │ Process  │                           │ Process  │
   └────┬─────┘                           └────┬─────┘
        │                                      │
        ▼                                      ▼
   ┌──────────┐                           ┌──────────┐
   │ Listen on│                           │ Listen on│
   │ Port 8080│                           │ Port 8081│
   │ & 8090   │                           └────┬─────┘
   └────┬─────┘                                │
        │                                      │
        ▼                                      ▼
   ┌──────────┐                           ┌──────────┐
   │ Create   │                           │ Load     │
   │ HDF5 File│                           │ files.   │
   │ in SWMR  │                           │ json     │
   │ Mode     │                           │ Config   │
   └────┬─────┘                           └────┬─────┘
        │                                      │
        ▼                                      ▼
   ┌──────────┐                           ┌──────────┐
   │ READY    │                           │ Init     │
   └──────────┘                           │ Circular │
                                          │ Buffers  │
                                          └────┬─────┘
                                               │
                                               ▼
                                          ┌──────────┐
                                          │ READY    │
                                          └──────────┘

      ▼                                            ▼
┌──────────────────┐                    ┌──────────────────┐
│  CLIENT          │                    │  CLIENT          │
│  CONTAINERS      │                    │  CONTAINERS      │
│  (depends_on)    │                    │  (robot clients) │
└────────┬─────────┘                    └────────┬─────────┘
         │                                       │
         ▼                                       ▼
   ┌──────────┐                           ┌──────────┐
   │ Wait for │                           │ Wait for │
   │ Marshal  │                           │ Marshal  │
   │ Health   │                           │ Ready    │
   └────┬─────┘                           └────┬─────┘
        │                                      │
        ▼                                      ▼
   ┌──────────┐                           ┌──────────┐
   │ Start    │                           │ Start    │
   │ Image    │                           │ Catheter │
   │ Streamer │                           │ Tracking │
   └────┬─────┘                           └────┬─────┘
        │                                      │
        ▼                                      ▼
   ┌──────────┐                           ┌──────────┐
   │ Start    │                           │ Start    │
   │ ECG      │                           │ Controlle│
   │ Client   │                           │ r        │
   └────┬─────┘                           └────┬─────┘
        │                                      │
        ▼                                      ▼
   ┌──────────┐                           ┌──────────┐
   │ Start    │                           │ Start    │
   │ Pose     │                           │ Planning │
   │ Client   │                           └────┬─────┘
   └────┬─────┘                                │
        │                                      ▼
        ▼                                 ┌──────────┐
   ┌──────────┐                           │ Start    │
   │ Start    │                           │ Front-End│
   │ Viz      │                           └────┬─────┘
   │ Client   │                                │
   └────┬─────┘                                ▼
        │                                 ┌──────────┐
        ▼                                 │ Start    │
   ┌──────────┐                           │ Surface  │
   │ ALL MRI  │                           │ Tracking │
   │ CLIENTS  │                           └────┬─────┘
   │ RUNNING  │                                │
   └──────────┘                                ▼
                                          ┌──────────┐
                                          │ ALL ROBOT│
                                          │ CLIENTS  │
                                          │ RUNNING  │
                                          └──────────┘

                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                   SYSTEM FULLY OPERATIONAL                       │
│                                                                  │
│  MRI Marshal:                                                    │
│    - HTTP API: http://localhost:8080                             │
│    - WebSocket: ws://localhost:8090/ws                           │
│                                                                  │
│  Robot Marshal:                                                  │
│    - HTTP API: http://localhost:8081                             │
│                                                                  │
│  Clients:                                                        │
│    - Streaming data at configured rates                          │
│    - Control loop running at 50-80 Hz                            │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Health Check Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                   DOCKER HEALTH CHECK                            │
└────────────────────────────┬────────────────────────────────────┘
                             │
                   ┌─────────┴──────────┐
                   │  Every 30 seconds  │
                   └─────────┬──────────┘
                             │
                             ▼
                   ┌──────────────────┐
                   │ curl GET         │
                   │ /health          │
                   └─────────┬────────┘
                             │
                  ┌──────────┴──────────┐
                  │                     │
                  ▼                     ▼
           ┌────────────┐        ┌────────────┐
           │  200 OK    │        │ Timeout or │
           │            │        │ Error      │
           └──────┬─────┘        └──────┬─────┘
                  │                     │
                  ▼                     ▼
           ┌────────────┐        ┌────────────┐
           │ Status:    │        │ Status:    │
           │ HEALTHY    │        │ UNHEALTHY  │
           └────────────┘        └──────┬─────┘
                                        │
                                        ▼
                              ┌──────────────────┐
                              │ After 3 Failures:│
                              │ Restart Container│
                              └──────────────────┘
```

---

## Summary

These flowcharts cover all major aspects of the CWRU Data Marshal system:

1. **MRI Marshal Request Flow** - How POST/GET requests are processed
2. **Robot Marshal Request Flow** - Read/write operations with background I/O
3. **Image Streaming Use Case** - End-to-end real-time imaging pipeline
4. **ECG Signal Processing** - Biological signal ingestion and distribution
5. **Catheter Control Loop** - 50-80 Hz coordination between 5 robot clients
6. **WebSocket Subscription Flow** - Pub/sub notification mechanism
7. **HDF5 SWMR Read/Write Flow** - Concurrent access patterns
8. **System Startup Sequence** - Docker Compose orchestration

All flowcharts use traditional symbols:
- **Rectangles**: Process steps
- **Diamonds**: Decision points
- **Arrows**: Flow direction
- **Double-line boxes**: Parallel operations
- **Thick boxes**: Subsystems/components

---

**Document Version:** 1.0
**Generated:** 2026-01-27
**Companion to:** ARCHITECTURE.md
