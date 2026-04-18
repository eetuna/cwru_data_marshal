# MRI Marshal Source-Only Independent Audit

**Date:** 2026-04-18  
**Scope:** MRI marshal server source only:
`.worktrees/mri_data_marshal/src` and marshal server headers under
`.worktrees/mri_data_marshal/include`.

**Out of scope:** clients, streamers, viz, robot marshal, Dockerfiles,
scripts, compose files, and existing docs/audit conclusions. Tests were used
only for verification, not as audit scope.

**Rule:** no code or document changes were made during source verification.
This document records the results afterward.

---

## Procedure

1. Inventory marshal server code only.
   - Main binary wiring from `.worktrees/mri_data_marshal/CMakeLists.txt`.
   - Server source files under `src/`.
   - Marshal support headers under `include/`.

2. Trace end-to-end server trust boundaries.
   - HTTP listener/session handling.
   - MRD TCP scanner listener.
   - Recon forwarder and recon return callback.
   - WebSocket server.
   - HDF5 sinks, live-image writers, latest-image writer, dump recorder.

3. Trace lifetime and threading.
   - Detached sessions.
   - Worker queues.
   - Callback ownership.
   - Socket shutdown and blocking writes.
   - Destructor and `close_scan()` behavior.

4. Trace wire parsing and allocation.
   - MRD tag dispatch.
   - Length-prefixed text/config/XML bodies.
   - Image attribute length and pixel-size calculations.
   - Waveform/acquisition sizing.
   - Parser overflow checks.

5. Trace persistence behavior.
   - HDF5 append paths.
   - Latest-image bulk and append paths.
   - Live-image recorder paths.
   - Dump recorder queue/drop semantics.

6. Run focused existing server-side verification.
   - Build targets: `marshal`, `test_mrd_sink`, `unit_http_handlers`, `it_http`.
   - Run test binaries: `unit_http_handlers`, `test_mrd_sink`, `it_http`.

7. Run parallel read-only verification agents.
   - Agent 1: threading/lifetime/shutdown.
   - Agent 2: wire parsing and integer/size boundaries.
   - Agent 3: persistence/queue/HDF5.
   - Agent 4: HTTP/WS/CLI.

---

## Verification Summary

### Build

The following build completed successfully:

```text
cmake --build build --target marshal test_mrd_sink unit_http_handlers it_http
```

Built targets:
- `marshal`
- `test_mrd_sink`
- `unit_http_handlers`
- `it_http`

### Tests

The following existing tests passed:

```text
./build/unit_http_handlers
```

Result: 53 assertions in 11 test cases.

```text
./build/test_mrd_sink
```

Result: 27 assertions in 8 test cases.

```text
./build/it_http
```

Result: 2 assertions in 2 test cases.

These tests cover happy paths only. They do not exercise shutdown races,
adversarial MRD wire lengths, malformed HDF5 image payloads, slow scanner
pushback, or memory-pressure behavior.

### Parallel Agent Status

Four read-only agents were launched:

| Slice | Status |
|-------|--------|
| Threading/lifetime/shutdown | Completed |
| Persistence/queue/HDF5 | Completed |
| HTTP/WS/CLI | Completed |
| Wire parsing and integer/size boundaries | Timed out; shut down |

The timed-out wire-parsing slice is still covered below as locally
source-verified, but not parallel-agent-confirmed.

---

## Confirmed Findings

### CRITICAL 1. Detached HTTP sessions can outlive `MarshalState`

**Status:** parallel-agent confirmed.

**Evidence:**
- `marshal_main.cpp:217` defines:
  `http_session(tcp::socket sock, MarshalState& state)`.
- `marshal_main.cpp:221-223` loops on blocking HTTP reads.
- `marshal_main.cpp:399-403` accepts sockets and starts detached threads:

```cpp
std::thread([s = std::move(sock), &state]() mutable {
    http_session(std::move(s), state);
}).detach();
```

- `marshal_main.cpp:414-418` closes only the HTTP acceptor, stops recon,
  flushes live lanes, closes scan state, and stops the `io_context`.

**Impact:** accepted HTTP sessions can keep using the stack-local
`MarshalState` after `main()` has returned or while shutdown is tearing down
state.

**Fix direction:** replace detached per-connection threads with tracked
session objects, close accepted sockets during shutdown, and join before
`MarshalState` destruction.

---

### CRITICAL 2. Detached MRD scanner sessions capture `this`

**Status:** parallel-agent confirmed.

**Evidence:**
- `mrd_tcp_listener.hpp:118` accepts scanner sockets.
- `mrd_tcp_listener.hpp:126-128` starts and detaches a thread capturing
  `this`:

```cpp
std::thread([this, socket_ptr]() {
    handle_session(socket_ptr);
}).detach();
```

- `mrd_tcp_listener.hpp:145` starts `handle_session()`, which uses listener
  members including `state_`, `forwarder_`, `scanner_mtx_`, and
  `scanner_socket_`.
- No destructor, stop method, session registry, or join path exists in
  `MrdTcpListener`.

**Impact:** a scanner session can outlive `MrdTcpListener`, `ReconForwarder`,
or `MarshalState`, causing use-after-free during shutdown.

**Fix direction:** make `MrdTcpListener` own tracked sessions, close session
sockets, signal shutdown, and join before listener/state destruction.

---

### CRITICAL 3. Shutdown order does not coordinate active sessions

**Status:** parallel-agent confirmed.

**Evidence:**
- `marshal_main.cpp:414-418` signal handler order:
  `acceptor.close()`, `forwarder->stop()`, `flush_all_live_lanes(state)`,
  `state.close_scan()`, `ioc.stop()`.
- `mrd_listener` is a local `std::unique_ptr` created at
  `marshal_main.cpp:378-381`, but the signal handler does not stop/reset it.
- Detached MRD sessions can call `forwarder_->end_session()` at
  `mrd_tcp_listener.hpp:395-396` while signal shutdown calls
  `forwarder->stop()` at `marshal_main.cpp:415`.
- Detached HTTP/MRD sessions can still reference `state` while
  `state.close_scan()` runs at `marshal_state.hpp:144-157`.

**Nuance:** `ReconForwarder` itself joins its reader thread; the unsafe part
is unmanaged accepted HTTP/MRD sessions and lack of coordination around
`MrdTcpListener` and shared state.

**Fix direction:** introduce a coordinated shutdown owner that stops
acceptors, closes active sockets, prevents new callbacks into state, and waits
for all sessions before clearing state.

---

### HIGH 4. Scanner pushback can block recon reader and shutdown

**Status:** parallel-agent confirmed.

**Evidence:**
- `recon_forwarder.hpp:274-275` invokes `on_message_()` synchronously from
  the recon reader thread.
- `marshal_main.cpp:333-339` callback first calls `state.mrd_push_message`.
- `marshal_main.cpp:383-384` routes that hook to
  `mrd_listener->push_message_to_scanner(...)`.
- `mrd_tcp_listener.hpp:69-86` holds `scanner_mtx_` while sending.
- `mrd_tcp_listener.hpp:102-114` loops on blocking `::send(...,
  MSG_NOSIGNAL)`.

**Impact:** a slow or stuck scanner can block the recon reader before
`handle_recon_image()` / `handle_recon_waveform()` side effects run. Shutdown
can also block waiting for the recon reader to join.

**Fix direction:** move scanner pushback to a bounded asynchronous writer or
nonblocking socket path with timeout/cancellation.

---

### HIGH 5. Recon image `attr_len` can overflow aggregate body size

**Status:** locally source-verified; wire-parsing agent timed out.

**Evidence:**
- `recon_forwarder.hpp:391-392` reads a 64-bit `attr_len` from the recon
  stream.
- `recon_forwarder.hpp:394-399` computes:

```cpp
size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes;
```

with no checked addition.

- `recon_forwarder.hpp:400` resizes `body` to `total`.
- `recon_forwarder.hpp:404` then reads `attr_len` bytes into
  `body.data() + off`.

**Impact:** hostile or corrupted recon data can wrap `total` small and then
copy/read past the end of the vector. This is stronger than a simple protocol
desynchronization concern on the recon-return path.

**Fix direction:** cap `attr_len`, checked-add `IMAGE_HEADER_BYTES`, length
fields, attributes, and pixels, and reject over-limit frames before allocation
or read.

---

### HIGH 6. Scanner-side image `attr_len` can overflow aggregate body size

**Status:** locally source-verified; wire-parsing agent timed out.

**Evidence:**
- `mrd_tcp_listener.hpp:331-334` reads `attr_len`, allocates `attr`, and reads
  that many bytes.
- `mrd_tcp_listener.hpp:343-344` computes:

```cpp
size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes;
std::vector<uint8_t> body(total);
```

with no checked addition.

- `mrd_tcp_listener.hpp:348` copies `attr_len` bytes into `body.data() + o`.

**Impact:** a scanner can provide a large `attr_len` that wraps `total` small,
then cause out-of-bounds writes while constructing `body`.

**Fix direction:** same checked-add/cap approach as recon image parsing.

---

### HIGH 7. Image pixel-size products are unchecked

**Status:** locally source-verified; wire-parsing agent timed out.

**Evidence:**
- `mrd_tcp_listener.hpp:335-339` computes scanner image `pixel_bytes` from
  matrix size, slices, channels, and data type with unchecked multiplication.
- `recon_forwarder.hpp:394-398` computes recon image `npixels` and
  `pixel_bytes` with unchecked multiplication.

**Impact:** overflow can produce a wrapped-small byte count, causing protocol
desynchronization, malformed image bodies, and unsafe downstream HDF5/live
image behavior.

**Fix direction:** centralize checked image-size calculation and enforce sane
dimension/data type caps.

---

### HIGH 8. Multiple scanner connections race through shared listener state

**Status:** source-verified from lifetime pass; not separately parallel
reported as its own item.

**Evidence:**
- `mrd_tcp_listener.hpp:117-130` accepts every scanner connection.
- `mrd_tcp_listener.hpp:122-125` overwrites shared `scanner_socket_`.
- `mrd_tcp_listener.hpp:100` has one shared `session_active_`.
- `mrd_tcp_listener.hpp:96-97` has one shared `state_` and `forwarder_`.

**Impact:** a second scanner connection can replace the pushback socket while
the first session still runs. Multiple sessions can mutate shared scan state
and recon session state concurrently.

**Fix direction:** reject concurrent scanner sessions or isolate all scanner
session state per connection.

---

### MEDIUM 9. HTTP body limit is enforced after `http::read()`

**Status:** parallel-agent confirmed.

**Evidence:**
- `marshal_main.cpp:222-223` reads the full HTTP request into
  `http::request<http::string_body>`.
- `marshal_main.cpp:228` then calls `handle_http_request`.
- `marshal_http.hpp:257-261` checks `req.body().size() > state.max_body_bytes`
  only inside the handler.

**Impact:** the configured marshal body limit does not prevent the read path
from allocating the body first.

**Nuance:** this confirms no marshal-configured parser `body_limit` is used
before reading. It does not make claims about Boost.Beast internals beyond
this code.

**Fix direction:** use `http::request_parser<http::string_body>` and set
`parser.body_limit(state.max_body_bytes)` before reading.

---

### MEDIUM 10. MRD text/config/XML length prefixes allocate directly from wire

**Status:** locally source-verified; wire-parsing agent timed out.

**Evidence:**
- `mrd_tcp_listener.hpp:197-200`, `221-224`, and `278-281` allocate
  `std::vector<uint8_t> body(4 + len)` from scanner-controlled `uint32_t len`.
- `recon_forwarder.hpp:359-365` does the same for recon-return
  length-prefixed bodies.

**Impact:** a peer can force large allocations or allocation failures before
the frame is rejected.

**Fix direction:** define per-message maximums and reject oversized frames
before allocation.

---

### MEDIUM 11. `attr_off + attr_len` parser checks are overflow-prone

**Status:** locally source-verified; wire-parsing agent timed out.

**Evidence:**
The following checks use `size < attr_off + attr_len`:
- `live_image_store.hpp:65`
- `live_image_recorder.cpp:30`
- `dump_recorder.cpp:246`
- `latest_image_writer.cpp:97`

**Impact:** if `attr_off + attr_len` wraps, malformed data can pass the
validation check and produce invalid offsets/pointers.

**Fix direction:** replace with:

```cpp
if (attr_len > size - attr_off) return false;
```

after first proving `size >= attr_off`.

---

### MEDIUM 12. LatestImageWriter queue is unbounded

**Status:** parallel-agent confirmed.

**Evidence:**
- `latest_image_writer.cpp:438` stores `std::deque<Job> jobs`.
- `latest_image_writer.cpp:456-463` pushes every job with no count or byte cap.
- `live_image_store.hpp:89-91` enqueues latest snapshots into
  `state.latest_writer`.

**Nuance:** destructor drains the queue at `latest_image_writer.cpp:446-453`;
the issue is memory/backlog growth during runtime.

**Fix direction:** add a bounded queue with explicit drop/coalesce/backpressure
policy. Latest image is naturally coalescible by lane/generation.

---

### MEDIUM 13. LiveImageRecorder queue is unbounded

**Status:** parallel-agent confirmed.

**Evidence:**
- `live_image_recorder.hpp:42-47` defines `std::deque<Job> queue_` with no
  limit.
- `live_image_recorder.cpp:60-68` pushes every job.
- `live_image_recorder.cpp:70-83` captures the full image body in the queued
  lambda.

**Nuance:** `close_scan()` is a barrier and waits for earlier writes, so a
large backlog also makes scan close slow.

**Fix direction:** bound by bytes/jobs and decide whether to block, drop, or
coalesce.

---

### MEDIUM 14. `MrdSink::append_image` trusts caller-provided `pixel_bytes`

**Status:** parallel-agent confirmed.

**Evidence:**
- `include/mrd_sink.hpp:45-48` exposes `pixel_bytes` as an API argument.
- `mrd_sink.cpp:106-190` dispatches by data type and copies exactly
  `pixel_bytes` into `img.getDataPtr()` with no recomputation.
- Representative copies: `mrd_sink.cpp:119`, `151`, `167`, `184`.

**Impact:** malformed upstream image body or overflowed size calculation can
violate the expected buffer size implied by the image header.

**Fix direction:** recompute expected bytes from `hdr` with checked arithmetic
and require an exact match before copying.

---

### MEDIUM 15. Latest-image bulk write uses unchecked size multiplication

**Status:** parallel-agent confirmed.

**Evidence:**
- `latest_image_writer.cpp:140-145` computes expected pixel bytes from header
  dimensions with unchecked multiplication.
- `latest_image_writer.cpp:341-343` allocates:

```cpp
pixel_bytes.resize(per_image_bytes * parsed_images.size());
```

with unchecked multiplication.

- `latest_image_writer.cpp:345-350` copies each image into that buffer.

**Nuance:** `can_bulk_write_latest_images()` validates matching
dimensions/type and that expected bytes equal payload bytes, but without
overflow checks.

**Fix direction:** use checked multiplication for both per-image and aggregate
buffer sizes, or fall back safely when products exceed limits.

---

### LOW 16. CLI numeric parsing is weak

**Status:** parallel-agent confirmed.

**Evidence:**
- `marshal_main.cpp:262-269` parses `--ws-port`, `--mrd-port`, and
  `--recon-port` using `std::stoi` then casts directly to `uint16_t`.
- `marshal_main.cpp:274-275` parses `--max-body-size` using `std::stoull`.
- No local catch or semantic validation surrounds those conversions.

**Nuance:** `--http` uses `parse_host_port()` and checks range, but its
`std::stoi` call does not check full-string consumption.

**Fix direction:** use checked parse helpers that reject trailing characters,
out-of-range values, and invalid input with clean usage errors.

---

## Rejected / Not Findings

### Production HTTP routes do not ingest image data

**Status:** parallel-agent rejected as a finding.

Evidence:
- `marshal_http.hpp:265-295` dispatches only:
  `/image/latest`, `/transform`, `/pose`, `/dump/scanner`, `/dump/recon`,
  and `/health`.
- `marshal_http.hpp:4-12` comments explicitly state scanner/recon data do not
  arrive via HTTP.

Image data enters through:
- scanner MRD path: `mrd_tcp_listener.hpp:327-360`
- recon return path: `recon_forwarder.hpp:385-407`, then
  `marshal_main.cpp:331-339`

### WebSocket binary ingest does not accept image data

**Status:** parallel-agent rejected as a finding.

Evidence:
- `marshal_ws.hpp:83-95` explicitly ignores binary messages and returns an
  acknowledgement.

### WebSocket send queue is bounded

**Status:** parallel-agent confirmed as bounded.

Evidence:
- `marshal_ws.hpp:234-235` defines `kMaxQueueSize = 1000`.
- `marshal_ws.hpp:322-333` drops new messages when the queue is full.

Nuance: read errors at `marshal_ws.hpp:254-258` are silently ignored and do
not immediately set `closed_`, but expired weak pointers are pruned during
client collection.

### ReconForwarder reader is not detached

**Status:** parallel-agent rejected as a detached-thread finding.

Evidence:
- `recon_forwarder.hpp:82` starts `reader_`.
- `recon_forwarder.hpp:93-109` shuts down the socket and joins `reader_`.
- `recon_forwarder.hpp:57` calls `end_session()` from the destructor.
- `recon_forwarder.hpp:179` makes `stop()` call `end_session()`.

Nuance: `end_session()` lacks a lifecycle mutex and can be invoked from
multiple threads, so a separate race audit is warranted.

### `LiveImageRecorder::close_scan()` is not a simple destructor-hang bug

**Status:** parallel-agent nuance confirmed.

Evidence:
- `live_image_recorder.cpp:86-103` enqueues a close job and waits.
- `live_image_recorder.cpp:117-121` catches `std::exception` from worker jobs.
- `live_image_recorder.cpp:49-58` destructor calls `close_scan()` before
  setting `stopping_`, then joins.

Nuance: `close_scan()` is still a synchronous barrier and can be slow under
backlog; it is not a standalone confirmed hang under normal handled
exceptions.

### DumpRecorder queue is bounded, not unbounded

**Status:** parallel-agent confirmed bounded/drop-on-overflow.

Evidence:
- `include/dump_recorder.hpp:82-83` sets `256 MiB` / `4096` job caps.
- `dump_recorder.cpp:37-56` drops queued work on overflow.
- `dump_recorder.cpp:137-145` records `dump_complete=false` and counters.

Nuance: lossy behavior is caller-invisible because enqueue returns `void`.

---

## Verification Gaps

The following are source-verified but need targeted tests/repros before
claiming runtime reproduction:

- shutdown with active keep-alive HTTP sessions
- shutdown with active MRD scanner session
- slow/stuck scanner blocking recon return pushback
- adversarial recon image `attr_len` overflow
- adversarial scanner image `attr_len` overflow
- image dimension/product overflow
- unbounded latest/live writer queue growth under slow HDF5 storage

---

## Suggested Fix Order

1. Replace detached HTTP and MRD sessions with tracked, stoppable sessions.
2. Add bounded, checked MRD frame parsing helpers for all length and size
   calculations.
3. Decouple scanner pushback from recon reader with bounded async writes.
4. Add checked image-size validation in `MrdSink::append_image()` and all
   latest/live/dump parse paths.
5. Bound or coalesce latest/live writer queues.
6. Clean up CLI parsing.

