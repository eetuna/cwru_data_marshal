# MRI Marshal — Merged Confirmed Bugs (final)

**Date:** 2026-04-18
**Branch:** `perf/latest-bulk-prealloc` (tip `1822829`) at `.worktrees/mri_data_marshal`
**Scope:** marshal server source only (`src/`, `include/`).
**Source audits merged:**
- [MRI_MARSHAL_BUG_AUDIT_2026-04-18.md](MRI_MARSHAL_BUG_AUDIT_2026-04-18.md) — Claude audit (13 confirmed after 4 verification rounds)
- [MRI_MARSHAL_SOURCE_ONLY_INDEPENDENT_AUDIT_2026-04-18.md](MRI_MARSHAL_SOURCE_ONLY_INDEPENDENT_AUDIT_2026-04-18.md) — Codex independent audit (16 findings)

Every item below was verified twice by independent agents against quoted source.

---

## CRITICAL — detached-thread UAF on shutdown

Fix as one refactor: replace `[&state]` / `[this]` + `.detach()` with tracked session objects owned by a long-lived component, close sockets on shutdown, join before `MarshalState` destruction.

### 1. UAF on MRD scanner session path
- **Site:** `src/mrd_tcp_listener.hpp:126-128` — `std::thread([this, socket_ptr](){ handle_session(socket_ptr); }).detach()`
- `MrdTcpListener` has compiler-generated destructor, no session registry, no join path. Sessions access `state_`, `forwarder_`, `scanner_mtx_`, `scanner_socket_` via `this`.
- **Impact:** UAF during shutdown; overlaps with #2 since this is the path that actually enqueues `publish_latest_snapshot` work.

### 2. UAF on MRD recon-return callback path
- **Site:** `src/live_image_store.hpp:82` (enqueue site), reached from `src/marshal_main.cpp:337` (recon return lambda).
- Completion lambda captures `[&state, generation]`. Detached MRD-TCP recon reader can enqueue new `publish_latest_snapshot` work during `MarshalState` teardown.
- Note: `LatestImageWriter` destructor drains already-queued jobs, so in-flight lambdas are safe. The hazard is **new work enqueued during main() return**.
- **Impact:** same class of UAF; only MRD paths reach this.

### 3. UAF on HTTP session path
- **Site:** `src/marshal_main.cpp:401-403` — `std::thread([s=..., &state]() mutable { http_session(std::move(s), state); }).detach()`.
- `http_session` loops on `http::read()`. Signal handler at 414-418 closes acceptor + stops ioc but does not join detached sessions.
- Routes affected: `/image/latest`, `/transform`, `/pose`, `/dump/*`, `/health`. HTTP does NOT reach `publish_latest_snapshot` (verified by call graph).
- **Impact:** UAF on query/control endpoints during shutdown.

---

## HIGH — wire-trust / concurrency / lifetime

### 4. Scanner pushback can block recon reader and shutdown
- **Sites:** `src/recon_forwarder.hpp:274-275` calls `on_message_()` synchronously in reader thread → `src/marshal_main.cpp:383-384` → `src/mrd_tcp_listener.hpp:69-86` (takes `scanner_mtx_`) → `src/mrd_tcp_listener.hpp:102-114` (blocking `::send` loop, `MSG_NOSIGNAL`, no timeout).
- `src/recon_forwarder.hpp:93-101` `end_session()` calls `reader_.join()` unconditionally.
- **Impact:** slow/stuck scanner blocks recon reader indefinitely; shutdown hangs on `reader_.join()` while reader is stuck in `::send`.
- **Fix:** async writer with bounded queue, or non-blocking send + timeout.

### 5. Recon image `attr_len` wrap → true OOB write
- **Sites:** `src/recon_forwarder.hpp:391-404`
- `uint64_t attr_len` read from wire.
- Line 399: `size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes` — unchecked.
- Line 400: `body.resize(total)` (wraps to small value with malicious `attr_len`).
- Line 404: `read_exact(body.data() + off, attr_len)` writes past `body.end()`.
- **Impact:** attacker-controlled OOB write on recon-return path.
- **Fix:** checked addition; cap `attr_len`; reject before allocation.

### 6. Scanner image `attr_len` wrap → true OOB write
- **Sites:** `src/mrd_tcp_listener.hpp:331-348` — mirror of #5.
- Line 343: `size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes` — unchecked.
- Line 344: `std::vector<uint8_t> body(total)`.
- Line 348: `memcpy(body.data() + o, attr.data(), attr_len)` writes past `body.end()`.
- **Impact:** attacker-controlled OOB write on scanner side.
- **Fix:** same as #5.

### 7. Image pixel-size products are unchecked
- **Sites:** `src/mrd_tcp_listener.hpp:335-339`, `src/recon_forwarder.hpp:394-398`.
- `pixel_bytes = matrix_size[0] * matrix_size[1] * max(matrix_size[2],1) * max(channels,1) * sizeof_datatype` — no overflow check.
- Combined with #5/#6 produces the OOB write chain; alone causes undersized allocation + protocol desync/DoS.
- **Fix:** centralized `checked_mul` helper + dimension/datatype caps.

### 8. Multiple scanner connections race through shared listener state
- **Sites:** `src/mrd_tcp_listener.hpp:117-130` — `do_accept()` accepts every connection and overwrites `scanner_socket_` at line 123.
- Single shared `session_active_` (line 100), `state_`/`forwarder_` (lines 96-97).
- **Impact:** a second scanner connection replaces the pushback socket while the first is still in use; concurrent mutation of shared scan state.
- **Fix:** reject concurrent scanner sessions, or isolate per-connection state.

### 9. `recon_group_is_complete` premature on uninitialized expected_slices
- **Site:** `src/live_image_store.hpp:105` — `if (state.recon_expected_slices <= 1) return true;`.
- `recon_expected_slices` default-initialised to 0 (`marshal_state.hpp:105`); only set when XML header parsed (`mrd_tcp_listener.hpp:231-252`).
- **Impact:** if first recon image arrives before XML (race / reconnect / malformed stream), first slice is published as a full multislice stack.
- **Fix:** explicit "header parsed" validity flag; do not use sentinel 0 as "complete".

### 10. Dump queue drops without caller-visible backpressure
- **Site:** `src/dump_recorder.cpp:42-50` — `enqueue()` returns `void`; on overflow drops queue, bumps counters, logs once (`drop_logged_.exchange(true)`), HDF5 gets `dump_complete="false"` on close (line 142).
- **Impact:** data loss with no runtime signal to caller.
- **Fix:** change `enqueue()` to return a result so caller can block/drop/escalate.

---

## MEDIUM — DoS / correctness / parser hygiene

### 11. HTTP body limit enforced after full read
- **Site:** `src/marshal_main.cpp:222-223` reads full `http::request<http::string_body>`; `src/marshal_http.hpp:257-261` checks `body.size() > state.max_body_bytes` inside handler.
- **Impact:** attacker can force allocation of up to whatever Beast reads before the handler sees it; limit is post-facto, not preventive.
- **Fix:** use `http::request_parser<http::string_body>` and `parser.body_limit(state.max_body_bytes)` BEFORE `http::read`.

### 12. MRD length-prefix bodies allocate directly from wire
- **Sites:** `src/mrd_tcp_listener.hpp:197-200`, `:221-224`, `:278-281`; `src/recon_forwarder.hpp:359-365`.
- `std::vector<uint8_t> body(4 + len)` allocated from scanner/recon-controlled `uint32_t len` (up to 4 GiB) before frame is validated.
- **Impact:** allocation DoS; malloc failure before frame rejection.
- **Fix:** per-message maximum; reject oversized frames before allocation.

### 13. `attr_off + attr_len` overflow-prone parser check
- **Sites:** `src/live_image_store.hpp:65`, `src/live_image_recorder.cpp:30`, `src/dump_recorder.cpp:246`, `src/latest_image_writer.cpp:97`.
- `if (size < attr_off + attr_len) return false;` — if the sum wraps, check passes on malformed data.
- **Fix:** replace with `if (attr_len > size - attr_off) return false;` after verifying `size >= attr_off`.

### 14. LatestImageWriter queue unbounded
- **Site:** `src/latest_image_writer.cpp:438` (`std::deque<Job> jobs`), enqueue at 456-463 with no cap. Each Job holds full image buffers.
- **Impact:** memory/backlog growth under slow HDF5 storage. Destructor drains but runtime is uncapped.
- **Fix:** bounded queue with explicit policy (drop, coalesce, backpressure). Latest is naturally coalescible.

### 15. LiveImageRecorder queue unbounded
- **Sites:** `src/live_image_recorder.hpp:42-47` (`std::deque<Job> queue_`), `live_image_recorder.cpp:60-68` push, `:70-83` lambda captures full image body.
- **Impact:** memory growth; `close_scan()` becomes slow barrier under backlog.
- **Fix:** bound by bytes/jobs; block, drop, or coalesce.

### 16. MrdSink::append_image trusts caller-provided `pixel_bytes`
- **Sites:** `include/mrd_sink.hpp:45-48` API; `mrd_sink.cpp:106-190` dispatches by data type and memcpys `pixel_bytes` without recomputing from header.
- **Impact:** malformed upstream image or overflowed size computation can memcpy past the buffer implied by the image header.
- **Fix:** recompute expected bytes from header with checked arithmetic; reject if mismatch.

### 17. Latest-image bulk write unchecked size multiplication
- **Sites:** `src/latest_image_writer.cpp:140-145` (per-image bytes), `:341-343` (`pixel_bytes.resize(per_image_bytes * parsed_images.size())`).
- **Impact:** wrapped aggregate → tiny buffer → OOB copy at `:345-350`.
- **Fix:** checked multiplication for both per-image and aggregate sizes.

### 18. Bare `catch (...) {}` in push-message callbacks
- **Sites:** `src/marshal_main.cpp:327`, `:334`.
- Silently swallow every exception from `mrd_push_message`.
- **Fix:** log and route to a metric.

---

## LOW / NIT

### 19. CLI numeric parsing is weak
- **Sites:** `src/marshal_main.cpp:263-269` (`std::stoi` → `uint16_t` with no check), `:274-275` (`std::stoull` with no check).
- `--http` uses `parse_host_port()` with range check, but `std::stoi` inside doesn't check full-string consumption.
- **Fix:** checked parse helper that rejects trailing chars / out-of-range / invalid input.

### 20. `attribute_string_len` silently truncates at 4 GB
- **Site:** `src/latest_image_writer.cpp:102` — `static_cast<uint32_t>(parsed.attributes.size())`.
- **Impact:** theoretical; ISMRMRD attributes are KB–MB. Low real-world risk.
- **Fix:** reject or clamp before cast.

### 21. Waveform size notation inconsistency
- **Sites:** `src/marshal_http.hpp:95` uses `sizeof(uint32_t)`, `src/mrd_tcp_listener.hpp:367` uses hardcoded `* 4`.
- Numerically equivalent. Divergence risk if wire format changes.

---

## Non-source items (compose, scripts)

Strictly out of the marshal-source audit scope but kept here as separate items:

### 22. `KSPACE_INTERVAL` default mismatch in compose
- `docker-compose.demo.yml:123,133` defaults `0.5`; `.env.demo` sets `0.033`. Scripts pass `--env-file` so scripted launches are correct. Manual `docker compose up` without env file → 2 FPS.

### 23. `DUMP_RECON_HOST` uses `-` instead of `:-`
- `docker-compose.demo.yml:48`. Valid syntax; differs from `:-` only on empty-but-set values. Stylistic.

### Redundancies
- **R1** — `scripts/demo-docker.sh` ↔ `scripts/demo-persistent.sh` ~78% duplicate.
- **R2** — `scripts/bench_fps.sh` exists byte-identical at both umbrella and inner paths.
- **R3** — `docker/Dockerfile.image-streamer` ↔ `docker/Dockerfile.kspace-streamer` ~17 lines duplicated.

---

## Docs action

### 24. `docs/DEVELOPER_GUIDE.md` stale branch diagram
- Lines 8-26 reference `main` / `mri-data-marshal` / `robot-data-marshal` umbrella layout. Current layout is `audit/mri-marshal-protocol-fixes-umbrella` + `perf/latest-*` experiment branches in `.worktrees/mri_data_marshal`.
- Rewrite to match current state; mark old layout as historical.

---

## Dropped in verification (not listed above)

Preserved in the source audit docs for history:
- `H5Fflush` missing before latest-image close — RAII close flushes; atomic rename targets closed tmp file; viz retries on parse failure.
- `mrd_sink.cpp:56-64` H5Fclose on `H5Gcreate2` failure — code does correctly close before throwing.
- `recon_forwarder` socket leak on `resolver.resolve` throw — `unique_ptr` RAII handles cleanup.
- `H5Tinsert` status chain clobber — ternary short-circuits correctly.
- `recon_forwarder::send_config_text` / `send_header` size overflow — strings are marshal-controlled, not wire.
- `mrd_tcp_listener` acquisition traj_bytes/sample_bytes overflow — bounded at ~786 KB by uint8×uint16×4.
- `close_scan()` destructor hang — worker catches std::exception; non-std exceptions `std::terminate`; no realistic hang path.

---

## Severity count

- **CRITICAL:** 3 (shutdown UAFs)
- **HIGH:** 7 (#4 blocking-send, #5/#6 OOB writes, #7 pixel overflow, #8 scanner race, #9 recon-group, #10 dump backpressure)
- **MEDIUM:** 8 (#11 HTTP body, #12 length-prefix alloc, #13 attr_off overflow, #14/#15 unbounded queues, #16 sink trust, #17 bulk-write mul, #18 catch-swallow)
- **LOW/NIT:** 3 (#19 CLI, #20 attr uint32, #21 notation)
- **Non-source:** 2 findings + 3 redundancies + 1 docs action

**Net: 21 source-code findings + 6 ops/docs items.**

---

## Recommended fix order

1. CRITICAL #1–3 together: tracked sessions, joined shutdown.
2. HIGH #4: async scanner writer with bounded queue + timeout.
3. HIGH #5/#6/#7: checked arithmetic + caps on all wire-size math.
4. HIGH #8: single-scanner-session enforcement or per-session state isolation.
5. HIGH #9: header-parsed validity flag.
6. HIGH #10: backpressure contract on `dump_recorder::enqueue`.
7. MEDIUM #11: Beast `body_limit()` before read.
8. MEDIUM #12–13: per-message max before allocation.
9. MEDIUM #14–15: refactor parser check + sink trust + bulk-write mul together.
10. MEDIUM #16–18: defensive checks + error visibility.
11. LOW/NIT cleanup.
12. Ops/docs items last.

Performance impact of all fixes: zero measurable FPS hit (all fixes are shutdown-only, per-frame O(1) checks, or bounded allocation gates).
