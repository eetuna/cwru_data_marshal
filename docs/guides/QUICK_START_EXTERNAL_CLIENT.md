# Quick Start: External Client Integration

This guide provides minimal, copy-pasteable examples for interacting with the CWRU Data Marshal from any external application (Python, MATLAB, C#, etc.).

---

## 1. Sending Data (HTTP POST)

### A. Update a Robot/Scanner Pose
```bash
curl -X POST http://localhost:8080/v1/pose/update \
  -H "Content-Type: application/json" \
  -d 
    "{
    "p": [1.0, 2.0, 3.0],
    "R": [1,0,0,0,1,0,0,0,1],
    "frame": "scanner",
    "source": "external_script"
  }"
```

### B. Ingest a Biological Signal (ECG/Resp)
```bash
curl -X POST http://localhost:8080/v1/bio/signal \
  -H "Content-Type: application/json" \
  -d 
    "{
    "ts": "2025-12-19T12:00:00.123Z",
    "source": "ecg",
    "data": [0.1, 0.8, 0.1],
    "rate_hz": 100
  }"
```

---

## 2. Pulling Data (HTTP GET)

### A. Get the Latest MRI Frame Metadata
```bash
curl http://localhost:8080/v1/mrd/latest
```

### B. Get Data History (Since a timestamp)
```bash
# Returns everything that happened after the given time
curl "http://localhost:8080/v1/mrd/since?ts=2025-12-19T12:00:00.000Z"
```

---

## 3. Real-time Subscriptions (WebSocket)

Connect to: `ws://localhost:8090/ws`

### Subscription Logic
You must send a JSON "Subscribe" message to receive specific traffic.

**To receive Poses:**
```json
{"subscribe": "pose"}
```

**To receive MRI Frame Notifications:**
```json
{"subscribe": "mrd"}
```

---

## 4. Working with Large Files (MRD)

If you have a completed `.mrd` file and want to archive it:
```bash
curl -H "Content-Type: application/octet-stream" \
     --data-binary @my_scan.mrd \
     http://localhost:8080/v1/mrd/ingest
```

---

## Technical Notes for Developers
- **Timestamp Format:** Always use ISO-8601 with milliseconds (e.g., `2025-12-19T12:00:00.123Z`).
- **Success Response:** Successful POSTs return `200 OK` with a JSON body containing `"status": "ok"`.
- **Error Format:** Errors return 4xx/500 with `{ "error": "message" }`.
