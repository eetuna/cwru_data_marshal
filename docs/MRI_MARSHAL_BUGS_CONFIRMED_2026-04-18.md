# MRI Marshal — Confirmed Bugs (clean list)

**Date:** 2026-04-18
**Branch audited:** `perf/latest-bulk-prealloc` (tip `1822829`) at `.worktrees/mri_data_marshal`
**Source audit:** [MRI_MARSHAL_BUG_AUDIT_2026-04-18.md](MRI_MARSHAL_BUG_AUDIT_2026-04-18.md) (full history including false positives and verification chain)

This doc is the **action list only**. 13 confirmed issues after 4 rounds of verification (Claude audit → Claude verification → Codex correction → Claude re-verification → Codex final correction → unanimous call-graph re-verification).

---

## CRITICAL — fix together as one rework

The three findings below are the same class of bug (UAF via
detached-thread-over-lifetime of stack-local `MarshalState`) on three
different call paths.

### 1. UAF on latest-image publish path

- **Site:** `src/live_image_store.hpp:82`
- **Mechanism:** Detached MRD scanner session threads and detached MRD
  recon-return threads can call `publish_latest_snapshot()` after
  `main()` has started tearing down `MarshalState`. The `on_complete`
  lambda captures `[&state, generation]` by reference; enqueued work
  that lands during teardown dereferences dead state.
- **Not reachable from:** any HTTP route (verified by call-graph trace).
- **Impact:** UAF on shutdown; intermittent crashes or silent memory
  corruption.

### 2. UAF on HTTP session path

- **Site:** `src/marshal_main.cpp:401-403`
- **Mechanism:** HTTP session threads spawned with `[&state]` capture
  and `.detach()`. `http_session` loops on `http::read()`. Shutdown
  (`acceptor.close()`, `ioc.stop()`) does not join or cancel them.
  Threads can still be in `http_session` after `main()` returns and
  destroys stack-local `MarshalState`.
- **Routes affected:** `/image/latest`, `/transform`, `/pose`,
  `/dump/scanner`, `/dump/recon`, `/health`.
- **Impact:** UAF on shutdown; no graceful close for in-flight HTTP.

### 3. UAF on MRD TCP listener

- **Site:** `src/mrd_tcp_listener.hpp:126-128`
- **Mechanism:** MRD session threads detached with `[this, socket_ptr]`
  capture. `MrdTcpListener` has the compiler-generated destructor. No
  tracked list of active sessions. Shutdown does not close the acceptor
  or signal sessions to stop. Detached threads access `state_` and
  `forwarder_` through `this` after listener destruction.
- **Impact:** Same class as #1/#2; more likely to trigger on
  protocol-level graceful scan close.

**Unified fix:** replace `[&state]` + `.detach()` with
`shared_ptr<MarshalState>` (or `MarshalState` owned by a long-lived
component) and track sessions so the shutdown path can join them.

---

## HIGH

### 4. Scanner-side image pixel_bytes overflow

- **Site:** `src/mrd_tcp_listener.hpp:335-339`
- **Mechanism:** Four `uint16_t` fields multiplied + sizeof(datatype up
  to 16). Unchecked. With max adversarial input the product overflows
  64-bit `size_t` and wraps to a small value. `std::vector<uint8_t>`
  allocated with the wrapped size; `read_exact` reads the wrapped-small
  count.
- **Impact:** Undersized allocation → protocol desynchronization → DoS.
  Not a direct OOB write (both vector and read use the same wrapped
  size).

### 5. Recon-return image pixel_bytes overflow

- **Site:** `src/recon_forwarder.hpp:394-398`
- **Mechanism:** Mirror of #4 on the scanner-return path.
- **Impact:** Same — undersized allocation + protocol desync / DoS.

**Fix:** explicit `checked_mul` style guard on all four size_t products
before allocation.

### 6. `recon_group_is_complete` premature on uninitialized expected_slices

- **Site:** `src/live_image_store.hpp:105`
- **Mechanism:** `recon_expected_slices` is default-initialised to 0
  (`src/marshal_state.hpp:105`) and only set from the parsed XML header
  (`src/mrd_tcp_listener.hpp:231-252`). The `<= 1` branch in
  `recon_group_is_complete` treats 0 as "complete". If the first recon
  image arrives before the header XML is parsed (race, reconnect,
  malformed stream), the first slice is published as a full stack.
- **Impact:** Latest image shows a partial stack as final; viz presents
  wrong data without error.

**Fix:** gate on an explicit "header parsed" validity flag, not on the
sentinel `== 0`.

### 7. Dump queue overflow drops without caller-visible backpressure

- **Site:** `src/dump_recorder.cpp:42-50`
- **Mechanism:** `enqueue()` returns `void`. On overflow, drops the
  queue and increments atomic counters. The first overflow logs
  `LOG_WARN` (guarded by `drop_logged_.exchange(true)`). On close, the
  HDF5 file gets `dump_complete="false"` attribute
  (`src/dump_recorder.cpp:142`). Caller has no runtime signal.
- **Impact:** Data loss under overload; caller cannot throttle or
  escalate.

**Fix:** change `enqueue()` return to signal overflow so the caller can
choose (block / drop / escalate).

---

## MEDIUM

### 8. Bare `catch (...) {}` in push-message callbacks

- **Sites:** `src/marshal_main.cpp:327` and `:334`
- **Mechanism:** Two callbacks silently swallow every exception from
  `mrd_push_message()`. No logging.
- **Impact:** Transport errors become invisible; debugging blindness.

**Fix:** at minimum log the exception; ideally propagate or route to a
metric.

### 9. Unchecked `std::stoi` on CLI port args

- **Sites:** `src/marshal_main.cpp:263-269`
- **Mechanism:** `static_cast<uint16_t>(std::stoi(argv[++i]))` throws
  `std::invalid_argument` / `std::out_of_range` on bad input. Unhandled
  in `main()`.
- **Impact:** Bad CLI input → process termination instead of a clean
  error message.

**Fix:** wrap CLI parsing in try/catch; emit a usage-style error and
exit 1.

### 10. `attribute_string_len` silently truncates at 4 GB

- **Site:** `src/latest_image_writer.cpp:102`
- **Mechanism:** `static_cast<uint32_t>(parsed.attributes.size())`
  wraps if attributes ≥ 2³² bytes. Theoretical — ISMRMRD attributes are
  typically KB–MB.
- **Impact:** Very low real-world risk. Should be defensively bounded.

**Fix:** reject or clamp before cast.

---

## LOW / NIT

### 11. Waveform size notation inconsistency

- **Sites:** `src/marshal_http.hpp:95` uses `sizeof(uint32_t)`;
  `src/mrd_tcp_listener.hpp:367` uses hardcoded `* 4`. Numerically
  identical.
- **Impact:** Style / maintenance risk if the wire format ever changes.

### 12. `KSPACE_INTERVAL` default mismatch in compose

- **Sites:** `docker-compose.demo.yml:123,133` default
  `${KSPACE_INTERVAL:-0.5}` (2 FPS); `.env.demo` sets `0.033` (30 FPS).
  Demo scripts always pass `--env-file .env.demo`, so scripted launches
  are correct.
- **Impact:** Only bites on manual `docker compose up` without env file.

**Fix:** set the compose default to match `.env.demo`.

### 13. `DUMP_RECON_HOST` single-dash expansion

- **Site:** `docker-compose.demo.yml:48` — `${DUMP_RECON_HOST-mock-recon}`
- **Mechanism:** Valid shell/compose syntax. `-` vs `:-` differ only
  for empty-but-set values; in this context both produce the same
  default.
- **Impact:** None observed; stylistic only.

---

## Confirmed redundancies

### R1. `scripts/demo-docker.sh` ↔ `scripts/demo-persistent.sh`

~78% duplicate code (config load, monitor loop, robot-ops counting).
Merge with `--persistent` flag or extract `common_demo.sh`.

### R2. `scripts/bench_fps.sh` duplicated at umbrella and inner paths

- `/workspaces/cwru_data_marshal/scripts/bench_fps.sh`
- `/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/scripts/bench_fps.sh`

Byte-identical. Keep the inner copy (which is where the binaries live)
and delete the umbrella copy, or symlink.

### R3. `docker/Dockerfile.image-streamer` ↔ `docker/Dockerfile.kspace-streamer`

~17 identical base-install lines of ~42. Multi-stage shared base would
save duplicated build work and disk.

---

## Docs action

### docs/DEVELOPER_GUIDE.md

Lines 8-26 reference the old `main` / `mri-data-marshal` /
`robot-data-marshal` umbrella layout. Current layout is
`audit/mri-marshal-protocol-fixes-umbrella` with experiment branches
`perf/latest-*` in `.worktrees/mri_data_marshal`. Rewrite lines 8-26 to
match current state; mark the old layout as historical.

---

## Recommended order

1. Fix CRITICAL #1–3 as one refactor (shared_ptr + tracked sessions +
   joined shutdown).
2. Add overflow guards for HIGH #4, #5.
3. Add header-validity flag for HIGH #6.
4. Change `dump_recorder::enqueue` contract for HIGH #7.
5. Fix MEDIUM #8–10 opportunistically.
6. Address redundancies R1–R3 in a cleanup pass.
7. Update DEVELOPER_GUIDE.md when convenient.
8. Reassess LOW/NIT after the above.

---

## What's NOT in this list (dropped during verification)

7 round-1 findings were dropped as false positives after two independent
verifications:

- `H5Fflush` missing before latest file close — RAII close is sufficient
  because rename targets the already-closed tmp file
- `mrd_sink` H5 handle leak on group-create failure — code does
  correctly close
- `recon_forwarder` socket leak on resolver throw — `unique_ptr` RAII
  handles it
- `H5Tinsert` status chain "clobber" — ternary short-circuits correctly
- `recon_forwarder::send_config_text` / `send_header` size overflow —
  not attacker-controlled
- `mrd_tcp_listener` acquisition `traj_bytes` overflow — bounded at
  ~786 KB
- `close_scan()` hang on worker death — worker catches `std::exception`;
  non-std exceptions call `std::terminate` (process crash, not hang)

See the full audit doc for the round-by-round evidence.
