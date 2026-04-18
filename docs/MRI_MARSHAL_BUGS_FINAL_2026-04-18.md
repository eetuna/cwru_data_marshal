# MRI Marshal — Final Confirmed Bugs (fix-ready)

**Date:** 2026-04-18
**Branch:** `perf/latest-bulk-prealloc` (tip `1822829`) at `.worktrees/mri_data_marshal`
**Scope:** marshal server source only (`src/`, `include/`). Non-source items (compose, scripts, docs) listed separately at the bottom.

**Source audits merged and reconciled:**
- [MRI_MARSHAL_BUG_AUDIT_2026-04-18.md](MRI_MARSHAL_BUG_AUDIT_2026-04-18.md) — Claude audit (13 confirmed after 4 verification rounds)
- [MRI_MARSHAL_SOURCE_ONLY_INDEPENDENT_AUDIT_2026-04-18.md](MRI_MARSHAL_SOURCE_ONLY_INDEPENDENT_AUDIT_2026-04-18.md) — Codex independent audit (16 findings)
- [MRI_MARSHAL_BUGS_CONFIRMED_MERGED_2026-04-18.md](MRI_MARSHAL_BUGS_CONFIRMED_MERGED_2026-04-18.md) — first merged draft (superseded by this doc)

**Verification provenance (honest):**
Most items verified by two or more independent agents against quoted source. A subset of wire-parsing items (Findings #5, #6, #7, #12, #13) had their parallel-agent pass time out during the codex audit and were source-verified locally only. Those are marked `[source-verified only]` inline. Subsequent Claude parallel-agent verification of Codex's corrections (on #4, #5, #6, #8, #12, #13, #14, #15, #16, #17) re-confirmed mechanism and quoted lines for those items — but the original wire-parsing slice itself did not complete in the codex run. Treat `[source-verified only]` as "probable, not yet cross-checked by a fresh independent reviewer."

---

## CRITICAL — detached-thread UAF on shutdown

Fix as one refactor: replace `[&state]` / `[this]` + `.detach()` with tracked session objects owned by a long-lived component. Close sockets on shutdown. Join sessions before `MarshalState` destruction.

### 1. UAF on MRD scanner session path
- **Site:** `src/mrd_tcp_listener.hpp:126-128` — `std::thread([this, socket_ptr](){ handle_session(socket_ptr); }).detach()`.
- `MrdTcpListener` has compiler-generated destructor, no session registry, no stop/join path. Sessions access `state_`, `forwarder_`, `scanner_mtx_`, `scanner_socket_` via `this`.
- **Impact:** UAF during shutdown; this path is also where scanner IMAGE → `append_live_image` → `publish_latest_snapshot` enqueue happens, so a session running during teardown can enqueue latest-image work against a dying state.

### 2. UAF via recon-return callback while lifetime is unmanaged
- **Enqueue site:** `src/live_image_store.hpp:82` — completion lambda captures `[&state, generation]`.
- **Trigger path:** `ReconForwarder::read_loop` (joined thread, not detached) invokes `on_message_` synchronously. Callback lives in `src/marshal_main.cpp:337` and routes IMAGE messages through `handle_recon_image` → `append_live_image(state, Recon, ...)` → `publish_latest_snapshot`.
- **Actual mechanism of the UAF:**
  - `ReconForwarder`'s reader thread is stored in `reader_` (`src/recon_forwarder.hpp:82`) and joined in `end_session()` (line 101), which is called from `stop()` (line 179) and from the destructor (line 57).
  - During shutdown, `forwarder->stop()` runs BEFORE `state.close_scan()` and `MarshalState` destruction (`src/marshal_main.cpp:414-418`). If `stop()` completes cleanly, no in-flight lambda sees a destroyed state — `LatestImageWriter`'s destructor also drains queued jobs before its sub-members are destroyed.
  - **Hazard 1:** if `stop()` is slow (e.g., reader stuck in scanner pushback — see #4), recon-side work can still reach `publish_latest_snapshot` after other shutdown steps have run.
  - **Hazard 2:** a scanner-side detached session (Finding #1) can independently trigger `append_live_image` → `publish_latest_snapshot` mid-teardown, reaching the same `on_complete` lambda with a dying state.
- **Impact:** UAF during shutdown, but only once #1 / #4 are exposed. Fixing #1 and adding a proper stop-before-teardown ordering for `forwarder` + sessions removes this class.

### 3. UAF on HTTP session path
- **Site:** `src/marshal_main.cpp:401-403` — `std::thread([s=..., &state]() mutable { http_session(std::move(s), state); }).detach()`.
- `http_session` loops on `http::read()` (`src/marshal_main.cpp:221-223`). Signal handler (`:414-418`) closes the acceptor and stops the io_context but does not join detached sessions.
- Routes affected: `/image/latest`, `/transform`, `/pose`, `/dump/scanner`, `/dump/recon`, `/health`. HTTP does **not** reach `publish_latest_snapshot` (verified by call graph).
- **Impact:** UAF on query/control endpoints during shutdown.

---

## HIGH — wire-trust / concurrency / lifetime

### 4. Scanner pushback can block recon reader and shutdown
- **Chain:**
  - `src/recon_forwarder.hpp:274-275` invokes `on_message_` synchronously from the reader thread.
  - Callback at `src/marshal_main.cpp:333-339` routes through `state.mrd_push_message`.
  - `state.mrd_push_message` (bound at `src/marshal_main.cpp:383-384`) calls `mrd_listener->push_message_to_scanner(...)`.
  - `src/mrd_tcp_listener.hpp:69-86` takes `scanner_mtx_` and calls `write_exact_fd`.
  - `src/mrd_tcp_listener.hpp:102-114` loops on blocking `::send(fd, ..., MSG_NOSIGNAL)` with no timeout.
- **Shutdown impact:** `src/recon_forwarder.hpp:93-101` `end_session()` calls `reader_.join()` unconditionally. If scanner is slow/stuck, reader is blocked inside `::send` holding `scanner_mtx_`, so shutdown hangs.
- **Fix:** async writer with bounded queue, or non-blocking send with timeout.

### 5. Recon-side image `attr_len` wrap → true OOB write  `[source-verified only]`
- **Site:** `src/recon_forwarder.hpp:391-404`.
- `uint64_t attr_len` read from wire.
- Line 399: `size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes` — unchecked.
- Line 400: `body.resize(total)` — wraps to a small value with a malicious `attr_len`.
- Line 404: `read_exact(body.data() + off, attr_len)` — reads `attr_len` bytes directly into `body`, past `body.end()`.
- No prior `attr` allocation gating the path. OOB write is reachable on the first hostile frame.
- **Impact:** attacker-controlled OOB write on recon-return path.
- **Fix:** checked addition, cap `attr_len`, reject oversize frames before allocation.

### 6. Scanner-side image `attr_len` wrap → allocation-gated OOB write  `[source-verified only]`
- **Site:** `src/mrd_tcp_listener.hpp:331-348`.
- Line 333: `std::vector<uint8_t> attr(attr_len)` — allocated **before** the aggregate body.
- Line 343: `size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes` — unchecked.
- Line 344: `std::vector<uint8_t> body(total)`.
- Line 348: `memcpy(body.data() + o, attr.data(), attr_len)`.
- **Qualification:** huge `attr_len` (e.g., 2⁶⁴−100) fails at `attr(attr_len)` allocation first — service is DoS'd, OOB write is never reached. OOB write only triggers for a narrower "goldilocks" `attr_len` that is small enough to allocate successfully (bounded by free RAM, ~tens of GB) but still causes `total` to wrap smaller than `IMAGE_HEADER_BYTES + 8 + attr_len`.
- **Impact:** allocation-gated OOB write on scanner side. The unchecked `total` math is still a real bug, even if the easiest exploit path reduces to DoS.
- **Fix:** same as #5.

### 7. Image pixel-size products are unchecked  `[source-verified only]`
- **Sites:** `src/mrd_tcp_listener.hpp:335-339`, `src/recon_forwarder.hpp:394-398`.
- `pixel_bytes = matrix_size[0] * matrix_size[1] * max(matrix_size[2],1) * max(channels,1) * sizeof_datatype` — no overflow check. All factors are attacker-controlled `uint16_t`.
- Combined with #5/#6 this is the overflow that produces the wrapped `total`. Alone it causes undersized allocation + protocol desync/DoS.
- **Fix:** centralised `checked_mul` helper plus dimension/datatype caps.

### 8. Multiple scanner connections race through shared listener state
- **Site:** `src/mrd_tcp_listener.hpp:117-130`. `do_accept()` accepts every connection and overwrites `scanner_socket_` at line 123.
- Single shared `session_active_` (`:100`), shared `state_` and `forwarder_` (`:96-97`).
- **Impact:** a second scanner connection replaces the pushback socket while the first session is still running in a detached thread; concurrent mutation of shared scan state.
- **Fix:** reject concurrent scanner sessions, or fully isolate per-connection state.

### 9. `recon_group_is_complete` premature on uninitialized `expected_slices`
- **Site:** `src/live_image_store.hpp:105` — `if (state.recon_expected_slices <= 1) return true;`.
- `recon_expected_slices` default-initialised to 0 (`src/marshal_state.hpp:105`); set only when the XML header is parsed (`src/mrd_tcp_listener.hpp:231-252`).
- **Impact:** if the first recon image arrives before XML (race, reconnect, malformed stream), the first slice is published as a complete multislice stack — viz shows wrong data with no error.
- **Fix:** explicit "header parsed" validity flag; do not use sentinel 0 as "complete".

### 10. Dump queue drops without caller-visible backpressure
- **Site:** `src/dump_recorder.cpp:42-50`. `enqueue()` returns `void`.
- On overflow: drops queue, increments `dropped_records_` / `dropped_bytes_`, logs once (`drop_logged_.exchange(true)`), and at close time writes `dump_complete="false"` (`:142`).
- **Impact:** the caller gets no runtime signal. Data loss is visible only in the closed HDF5 file attribute.
- **Fix:** change `enqueue()` to return a result so the caller can block / drop / escalate.

---

## MEDIUM — DoS, correctness, parser hygiene

### 11. HTTP body limit enforced after full read
- **Site:** `src/marshal_main.cpp:222-223` reads full `http::request<http::string_body>`; `src/marshal_http.hpp:257-261` checks `req.body().size() > state.max_body_bytes` inside the handler.
- Boost.Beast's default `http::string_body` has no built-in size limit; the configured `state.max_body_bytes` is post-facto.
- **Impact:** an attacker can force allocation of the full body before the handler sees it. DoS vector.
- **Fix:** use `http::request_parser<http::string_body>` and set `parser.body_limit(state.max_body_bytes)` before `http::read`.

### 12. MRD length-prefix bodies allocate directly from wire  `[source-verified only]`
- **Sites:** `src/mrd_tcp_listener.hpp:197-200`, `:221-224`, `:278-281`; `src/recon_forwarder.hpp:359-365`.
- `std::vector<uint8_t> body(4 + len)` allocated from scanner/recon-controlled `uint32_t len` (up to 4 GiB) before frame is validated.
- **Impact:** allocation DoS; malloc failure or multi-GB allocation before frame is even typed.
- **Fix:** per-message maximum; reject oversized frames before allocation.

### 13. `attr_off + attr_len` overflow-prone parser check  `[source-verified only]`
- **Sites:** `src/live_image_store.hpp:65`, `src/live_image_recorder.cpp:30`, `src/dump_recorder.cpp:246`, `src/latest_image_writer.cpp:97`.
- `if (size < attr_off + attr_len) return false;` — if the sum wraps, check passes on malformed data.
- **Fix:** replace with `if (attr_len > size - attr_off) return false;` after verifying `size >= attr_off`.

### 14. `LatestImageWriter` queue unbounded
- **Site:** `src/latest_image_writer.cpp:438` (`std::deque<Job> jobs`), enqueue at `:456-463` with no cap. Each `Job` holds full image buffers.
- **Impact:** memory/backlog growth under slow HDF5 storage. Destructor drains but runtime is uncapped.
- **Fix:** bounded queue with explicit policy. Latest-image is naturally coalescible by lane/generation.

### 15. `LiveImageRecorder` queue unbounded
- **Sites:** `src/live_image_recorder.hpp:42-47` (`std::deque<Job> queue_`), `src/live_image_recorder.cpp:60-68` push, `:70-83` lambda captures full image body.
- **Impact:** memory growth; `close_scan()` becomes a slow barrier under backlog.
- **Fix:** bound by bytes/jobs; block, drop, or coalesce.

### 16. `MrdSink::append_image` trusts caller-provided `pixel_bytes`
- **Sites:** `include/mrd_sink.hpp:45-48` (API); `src/mrd_sink.cpp:106-190` dispatches by data type and `memcpy`s `pixel_bytes` without recomputing from header.
- **Impact:** malformed upstream image or overflowed size computation can copy past the buffer implied by the image header.
- **Fix:** recompute expected bytes from header with checked arithmetic; reject on mismatch.

### 17. Latest-image bulk write uses unchecked size multiplication
- **Sites:** `src/latest_image_writer.cpp:140-145` (per-image bytes), `:341-343` (`pixel_bytes.resize(per_image_bytes * parsed_images.size())`).
- **Impact:** wrapped aggregate → tiny buffer → OOB copy at `:345-350`.
- **Fix:** checked multiplication for both per-image and aggregate sizes.

### 18. Bare `catch (...) {}` in push-message callbacks
- **Sites:** `src/marshal_main.cpp:327`, `:334`.
- Silently swallow every exception from `mrd_push_message`.
- **Fix:** log the exception at minimum; route to a metric ideally.

---

## LOW / NIT

### 19. CLI numeric parsing is weak
- **Sites:** `src/marshal_main.cpp:263-269` (`std::stoi` → `uint16_t` cast, no check); `:274-275` (`std::stoull` no check).
- `--http` uses `parse_host_port()` with range check, but the underlying `std::stoi` does not check full-string consumption.
- **Fix:** checked parse helper that rejects trailing characters, out-of-range values, invalid input.

### 20. `attribute_string_len` silently truncates at 4 GB
- **Site:** `src/latest_image_writer.cpp:102` — `static_cast<uint32_t>(parsed.attributes.size())`.
- **Impact:** theoretical; ISMRMRD attributes are KB–MB in practice.
- **Fix:** reject or clamp before cast.

### 21. Waveform size notation inconsistency
- **Sites:** `src/marshal_http.hpp:95` uses `sizeof(uint32_t)`; `src/mrd_tcp_listener.hpp:367` uses hardcoded `* 4`.
- Numerically identical. Divergence risk if the wire format ever changes.

---

## Non-source items (compose, scripts, docs)

### 22. `KSPACE_INTERVAL` default mismatch in compose
- `docker-compose.demo.yml:123,133` defaults to `${KSPACE_INTERVAL:-0.5}` (2 FPS); `.env.demo` sets `0.033` (30 FPS). Demo scripts always pass `--env-file .env.demo` so scripted launches are correct. Manual `docker compose up` without env file → 2 FPS.
- **Fix:** set the compose default to match `.env.demo`.

### 23. `DUMP_RECON_HOST` uses `-` instead of `:-`
- `docker-compose.demo.yml:48` — `${DUMP_RECON_HOST-mock-recon}`. Valid syntax; differs from `:-` only on empty-but-set values. Stylistic.

### Redundancies
- **R1** — `scripts/demo-docker.sh` ↔ `scripts/demo-persistent.sh` ~78% duplicate. Merge with a `--persistent` flag or extract `common_demo.sh`.
- **R2** — `scripts/bench_fps.sh` exists byte-identical at both umbrella (`scripts/bench_fps.sh`) and inner (`.worktrees/mri_data_marshal/scripts/bench_fps.sh`) paths. Keep the inner copy; symlink or delete the umbrella copy.
- **R3** — `docker/Dockerfile.image-streamer` ↔ `docker/Dockerfile.kspace-streamer` ~17 identical lines. Extract shared base or multi-stage build.

### 24. Docs — `docs/DEVELOPER_GUIDE.md` stale branch diagram
- Lines 8-26 reference `main` / `mri-data-marshal` / `robot-data-marshal`. Current layout is `audit/mri-marshal-protocol-fixes-umbrella` + `perf/latest-*` experiment branches in `.worktrees/mri_data_marshal`.
- **Fix:** rewrite lines 8-26 to match current state; mark old layout as historical.

---

## Dropped in verification — preserved for history

These were raised by round-1 auditors and rejected by independent verification with quoted-code evidence:

- `H5Fflush` missing before latest-image close — RAII `H5Fclose` flushes; atomic rename targets the closed tmp file; viz retries on parse failure. Adding explicit flush per frame would cost 5–50 ms per frame and defend against a scenario that cannot produce a corrupt production file.
- `mrd_sink.cpp:56-64` H5Fclose on `H5Gcreate2` failure — code already closes before throwing.
- `recon_forwarder` socket leak on `resolver.resolve` throw — `unique_ptr` RAII handles it.
- `H5Tinsert` status chain clobber — ternary short-circuits correctly, no further `H5Tinsert` runs after first failure.
- `recon_forwarder::send_config_text` / `send_header` size cast overflow — strings are marshal-controlled, not wire-controlled.
- `mrd_tcp_listener` acquisition `traj_bytes` / `sample_bytes` overflow — bounded at ~786 KB by `uint8_t × uint16_t × 4`.
- `close_scan()` destructor hang — worker catches `std::exception`; non-std exceptions trigger `std::terminate` (process crash, not destructor hang); destructor enqueues close job before setting `stopping_` so promise is always fulfilled on the happy path.

---

## Severity count

- **CRITICAL:** 3 (shutdown UAFs)
- **HIGH:** 7 (blocking send; unqualified OOB write on recon; allocation-gated OOB write on scanner; pixel-size overflow; scanner race; recon-group false-complete; dump backpressure)
- **MEDIUM:** 8 (HTTP body-limit timing; length-prefix alloc; attr_off overflow; two unbounded queues; sink-trust; bulk-write mul; catch-swallow)
- **LOW/NIT:** 3 (CLI parsing, attribute uint32 truncation, waveform notation)
- **Non-source:** 3 findings + 3 redundancies + 1 docs action

**Total: 21 source-code findings + 7 ops/docs items.**

---

## Recommended fix order

1. **CRITICAL #1 + #2 + #3 together.** Replace `[&state]` / `[this]` + `.detach()` with tracked session objects. Close sockets on shutdown. Join before `MarshalState` destruction. Fixes all three UAF classes.
2. **HIGH #4.** Async scanner writer with bounded queue and send timeout. Also removes the shutdown-hang interaction with #2.
3. **HIGH #5, #6, #7.** Centralised `checked_mul` / `checked_add` + caps on every wire-size math site.
4. **HIGH #8.** Single-scanner-session enforcement, or full per-session state isolation.
5. **HIGH #9.** Header-parsed validity flag.
6. **HIGH #10.** Return-value contract on `dump_recorder::enqueue`.
7. **MEDIUM #11.** Beast `body_limit()` set on the parser before `http::read`.
8. **MEDIUM #12.** Per-message max before allocation.
9. **MEDIUM #13 + #16 + #17 together.** Parser overflow check + sink trust + bulk-write multiplication — all share the same "checked arithmetic before allocation/copy" shape.
10. **MEDIUM #14 + #15.** Bounded queues with coalesce/drop policy.
11. **MEDIUM #18.** Log + metric in the two bare catch sites.
12. **LOW/NIT #19–21** cleanup.
13. Ops/docs items (#22–24, R1–R3) last.

### Performance note

The fixes are either shutdown-only (#1–3), per-session one-time cost (#4, #8), per-frame O(1) checks (#5–7, #13, #16, #17), or bounded-allocation gates (#11, #12, #14, #15). Aggregate per-frame overhead is on the order of a few hundred CPU cycles (<0.001% of the 50–67 million cycles available at 40 FPS). **Expected: negligible FPS impact. Validate with `scripts/bench_fps.sh` before and after each batch of fixes.** Do not claim "zero measurable" without the benchmark numbers.

---

## Verification provenance per item (quick reference)

| Finding | Primary verifier | Cross-check |
|---------|------------------|-------------|
| #1 | Claude + Codex parallel agents | Call-graph trace confirmed MRD enqueue path |
| #2 | Codex + Claude re-verification | Reader-not-detached mechanism confirmed |
| #3 | Claude + Codex parallel agents | HTTP routes confirmed do-not-reach-`publish_latest_snapshot` |
| #4 | Codex parallel agent | Claude re-verified chain with quoted `::send` + `reader_.join()` |
| #5 | Local source | (codex wire-parsing agent timed out) |
| #6 | Local source | Claude re-verified allocation-gating nuance |
| #7 | Local source | (codex wire-parsing agent timed out) |
| #8 | Claude source-verified | Claude parallel-confirmed |
| #9 | Claude parallel agent | Codex independently flagged same logic |
| #10 | Claude + Codex parallel agents | Confirmed warn-once + `dump_complete="false"` |
| #11 | Codex parallel agent | Beast `body_limit` check confirmed |
| #12 | Local source | (codex wire-parsing agent timed out) |
| #13 | Local source | (codex wire-parsing agent timed out) |
| #14 | Codex parallel agent | Claude re-verified unbounded deque |
| #15 | Codex parallel agent | Claude re-verified unbounded deque + body capture |
| #16 | Codex parallel agent | Claude re-verified `memcpy` without recomputation |
| #17 | Codex parallel agent | Claude re-verified multiplication + OOB copy |
| #18 | Claude parallel agent | Codex consistent |
| #19 | Claude + Codex parallel agents | CLI stoi path confirmed |
| #20 | Claude parallel agent | Cast path confirmed |
| #21 | Claude parallel agent | Both sites quoted |
| #22–24, R1–R3 | Claude scripts-audit agent | Codex non-source scope excluded |
