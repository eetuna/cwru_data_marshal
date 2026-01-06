# Client integration guide

This guide explains how external applications interact with the CWRU Data Marshal.
It complements the quick-start runbooks and API reference by focusing on the life
cycle of client connections, the reasoning behind each transport, and the
operational guardrails needed when deploying the marshal in safety-critical
robotic surgical environments.

## 1. Operating modes

The marshal always runs in exactly one of two sink modes:

| Mode | Purpose | Typical deployment | HTTP write path |
| ---- | ------- | ------------------ | --------------- |
| **Mode A – MRD sink** | Live scanner data flows into SWMR-enabled `.mrd` files that clients open directly from disk. | Online recon/visualization workstations consuming the latest slices. | `/v1/mrd/ingest` for whole files, `/v1/ismrmrd/frame` for incremental SWMR writes. |
| **Mode B – Dumpbox sink** | Incoming payloads are captured into a session directory for later replay. The marshal never touches `/data/mrd` in this mode. | Procedural capture or QA shifts where data is reviewed offline. | Same HTTP endpoints, but data lands under `dumpbox/<session>/files`. |

`marshal --sink mrd` selects Mode A. `marshal --sink dumpbox` (optionally
with `--dumpbox-session <name>`) selects Mode B. The active sink controls where
metadata files (`index.jsonl`, `latest.json`) live and therefore what clients
should poll.

## 2. Transport overview

### HTTP (ingest + control)

HTTP handles every state-changing action:

* Uploading complete `.mrd` files (`POST /v1/mrd/ingest`).
* Streaming ISMRMRD Image messages (`POST /v1/ismrmrd/frame`).
* Recording the latest robot/scanner pose (`POST /v1/pose/update`).

The marshal uses Boost.Beast’s synchronous server stack (`http::async_read`
parsers within a per-connection session object) and enforces a 128 MiB request
body limit to bound resource usage.【F:src/marshal_http.hpp†L47-L124】

Clients should keep HTTP requests small and idempotent:

* Use HTTP/1.1 keep-alive (default in Beast) for bursts of frames; the marshal
  will honour it and only closes the connection on errors or when the peer
  requests it.【F:clients/image_streamer/image_streamer_main.cpp†L166-L233】
* Handle `503` or network resets by reconnecting and retrying the frame; the
  C++ streamer already loops with exponential-ish backoff.【F:clients/image_streamer/image_streamer_main.cpp†L108-L158】【F:clients/image_streamer/image_streamer_main.cpp†L201-L233】
* Watch the JSON response: `"flushed": false` means the frame is durable on
  disk but not yet visible to SWMR readers (flush batching is governed by
  `--flush-max-frames`/`--flush-max-ms`).【F:docs/API_REFERENCE.md†L61-L110】

### WebSocket (metadata fan-out)

The WebSocket listener accepts connections on `ws://<bind>/ws` (default port
8090). It exists purely for broadcast metadata: every ingest/update event is
serialized once and fan-out happens in-process so clients are not forced to
poll `latest.json` aggressively.【F:src/marshal_ws.hpp†L18-L125】

WebSocket handshake steps:

1. Client opens a TCP connection and performs the standard HTTP upgrade.
2. On success the marshal stores the session handle and immediately starts
   reading for control messages or binary ingest payloads.【F:src/marshal_ws.hpp†L56-L111】
3. Optional: send `{"subscribe":"<topic>"}` (text frame) to filter updates by
   logical stream/topic. Empty or missing topic = receive all broadcasts.【F:src/marshal_ws.hpp†L83-L102】
4. Marshal emits JSON lines terminated by `\n`; keep-alive is handled via
   Beast’s default ping/pong timers. Clients should reply to ping frames or
   disconnect cleanly.

Binary frames posted over the WebSocket are treated as raw MRD payloads and
routed through the same `mrd::ingest_payload` helper as HTTP uploads, enabling
specialized acquisition rigs that already speak WebSocket.【F:src/marshal_ws.hpp†L106-L119】

## 3. Typical client workflows

### 3.1 File-based readers (polling or event-driven)

* Watch `latest.json` for stream metadata and the `flushed` flag. Polling every
  250 ms is usually sufficient when WebSockets are unavailable.【F:docs/API_REFERENCE.md†L93-L110】
* Open MRD files using `H5F_ACC_SWMR_READ`. Always reopen after the marshal
  reports a geometry change (filenames encode size/channel counts).【F:docs/API_REFERENCE.md†L61-L92】
* Handle the case where the marshal switches modes—`index.jsonl`/`latest.json`
  will move between `data/mrd` and `data/dumpbox/<session>`.

### 3.2 Streaming writers (C++ `image_streamer`, Python helpers)

* Keep the HTTP connection alive and reuse it. If the marshal closes the socket
  (e.g., after an idle timeout), reconnect before retrying the frame.
* For large payloads, drive a `http::request_serializer` with `serializer.split(true)`
  so Beast emits the body in multiple buffers and avoids `need_buffer` errors.【F:clients/image_streamer/image_streamer_main.cpp†L209-L233】
* Retry logic should bound the number of attempts (the stock streamer retries up
  to three times, reconnecting between tries) and log the marshal’s response
  body when the status is not `200 OK` so operators can triage quickly.【F:clients/image_streamer/image_streamer_main.cpp†L196-L233】

### 3.3 Pose publishers / robotics middleware bridges

* Construct a JSON payload with translation `p` (meters) and rotation matrix
  `R` (row-major) arrays. The marshal validates array lengths before updating
  the shared pose store and broadcasting to WebSocket subscribers.【F:docs/API_REFERENCE.md†L112-L148】【F:src/marshal_http.hpp†L174-L260】
* When bridging from ROS2 or proprietary robotics middleware, treat the marshal
  as the authoritative pose cache. Periodically poll `/v1/pose/current` to
  detect missed WebSocket updates or transport failures.【F:docs/API_REFERENCE.md†L150-L170】

## 4. Failure modes and resilience strategies

| Symptom | Likely cause | Mitigation |
| ------- | ------------ | ---------- |
| HTTP `payload_too_large` response | Client exceeded the 128 MiB limit. | Split uploads or stream via `/v1/ismrmrd/frame` with smaller voxel volumes.【F:src/marshal_http.hpp†L47-L124】 |
| `boost::beast::http::error::need_buffer` in legacy clients | Beast 1.83 removed the `flat_buffer` overload. | Upgrade to the new serializer-based implementation or fall back to `http::vector_body` (extra copy).【F:clients/image_streamer/image_streamer_main.cpp†L209-L233】【F:TROUBLESHOOTING.md†L15-L18】 |
| WebSocket disconnects after idle period | Missing ping/pong handling or network idle timeout. | Ensure clients reply to pings and reconnect automatically; monitor heartbeats in surgery control rooms.【F:src/marshal_ws.hpp†L65-L95】 |
| MRD readers see stale geometry | Marshal rolled to a new file after geometry change. | Watch metadata broadcasts/`latest.json` and reopen files when the `path` changes.【F:docs/API_REFERENCE.md†L61-L110】 |
| Dumpbox session missing files | Marshal running in Mode A while capture expected Mode B. | Confirm CLI flags; use `marshal --sink dumpbox --dumpbox-session <id>` during captures.【F:src/marshal_main.cpp†L23-L83】 |

## 5. Robotic surgical deployment considerations

Robotic surgical suites add constraints beyond typical research clusters:

* **Deterministic latency:** Use `--flush-max-frames 1 --flush-max-ms 0` in live
  Mode A when latency trumps throughput so visualization/robot controllers see
  every frame as soon as it is written.【F:README.md†L33-L60】
* **Network segregation:** Terminate WebSocket and HTTP ingress on a medically
  certified firewall or VLAN. The marshal does not implement authentication, so
  perimeter controls are mandatory.【F:docs/API_REFERENCE.md†L15-L23】
* **Redundant monitoring:** Feed `/health` and `/v1/pose/current` into the OR’s
  supervisory control system to detect hangs before they impact surgeons.
* **Fail-safe clients:** Surgical robots should treat missing or delayed frames
  as a non-fatal condition—hold the last known good dataset and alert the
  operator rather than extrapolating blindly.
* **Replay workflows:** For QA or incident review, capture sessions with the
  dumpbox sink, archive the `dumpbox/<session>` folder, and later use the
  `playback` utility to reproduce the exact HTTP sequence that fed the live
  marshal.【F:README.md†L7-L18】【F:README_MODE_B.md†L1-L120】

## 6. Checklist for new clients

1. **Decide on transport:** HTTP only (poll metadata) or HTTP + WebSocket for
   low-latency updates.
2. **Respect limits:** stay under 128 MiB per request; if you need larger
   volumes, chunk them.
3. **Implement retries:** handle network errors and marshal restarts gracefully.
4. **Watch metadata:** monitor `path`, `frame_index`, `flushed`, and timestamp
   fields to stay synchronized with the marshal’s view of the world.【F:docs/API_REFERENCE.md†L61-L110】
5. **Validate payloads before sending:** match the ISMRMRD header to the voxel
   data size; the marshal rejects mismatched lengths with `400 Bad Request`.【F:docs/API_REFERENCE.md†L61-L110】
6. **Plan for failover:** in surgical deployments, rehearse manual switchover to
   a standby marshal or local capture node to avoid data loss during outages.

Following this guide keeps integration straightforward while maintaining the
safety and predictability needed in clinical robotics programs.
