# MRI Marshal — Code Bug Audit (verified)

**Date:** 2026-04-18
**Scope:** marshal transport/HTTP/storage layer + scripts/compose/Dockerfiles
**Branch audited:** `perf/latest-bulk-prealloc` (tip `1822829`) at `.worktrees/mri_data_marshal`
**Method:** two-pass audit. Round 1 produced 25 candidate findings via three parallel auditors. Round 2 ran a fresh verification agent per finding with no prior context, requiring quoted-code evidence. Only findings verified twice are listed as CONFIRMED. False positives kept at bottom for independent re-investigation by another agent (e.g. codex).

---

## CONFIRMED bugs

### CRITICAL

#### 1. `live_image_store.hpp:82` — lambda captures MarshalState by reference, async use-after-free

```cpp
auto on_complete = [&state, generation](const std::filesystem::path& path) {
    // ...
};
state.latest_writer->enqueue(dest, std::move(xml), std::move(images), std::move(on_complete));
```

The callback captures `&state` by reference and is enqueued to a worker
thread that runs asynchronously. Worker invocation at
`latest_image_writer.cpp:479`:

```cpp
if (job.completion) job.completion(job.dest);
```

`MarshalState` is a stack-local in `main()` (`marshal_main.cpp:286`).
Detached HTTP handler threads (`marshal_main.cpp:401-403`) can call
`publish_latest_snapshot()` after the signal handler begins shutdown. Those
calls enqueue lambdas capturing `&state`. When `main()` returns and
destroys `state`, any still-pending or in-flight lambda executes with a
dangling reference.

**Impact:** UAF on shutdown, intermittent crashes or silent memory
corruption.

---

#### 2. `marshal_main.cpp:401-403` — HTTP session threads detached with `&state` capture

```cpp
std::thread([s = std::move(sock), &state]() mutable {
    http_session(std::move(s), state);
}).detach();
```

`http_session` loops on `http::read()` indefinitely. On shutdown,
`acceptor.close()` and `ioc.stop()` do not join or cancel these detached
threads. Threads can still be in `http_session` after `main()` returns and
destroys the stack-local `MarshalState`.

**Impact:** UAF on shutdown; no graceful close semantic for in-flight
requests.

---

#### 3. `mrd_tcp_listener.hpp:126-128` — MRD session threads detached with no tracking

```cpp
std::thread([this, socket_ptr]() {
    handle_session(socket_ptr);
}).detach();
```

`MrdTcpListener` has the compiler-generated destructor (no join). No
`std::vector` / `std::set` of active sessions. Shutdown at
`marshal_main.cpp:418` calls `ioc.stop()` but does not close the acceptor,
signal sessions, or wait. Detached threads access `state_` and `forwarder_`
through `this` after listener destruction.

**Impact:** Same class of UAF as #1 and #2; more likely to manifest on
protocol-level graceful shutdown of a scan.

---

### HIGH

#### 4. `mrd_tcp_listener.hpp:335-339` — image pixel_bytes overflow on adversarial header

```cpp
size_t pixel_bytes = size_t(ihdr->matrix_size[0]) * ihdr->matrix_size[1] *
                     std::max<uint16_t>(ihdr->matrix_size[2], 1) *
                     std::max<uint16_t>(ihdr->channels, 1) *
                     ISMRMRD::ismrmrd_sizeof_data_type(ihdr->data_type);
```

Four `uint16_t` fields multiplied + sizeof(datatype up to 16) can reach
~1.85×10¹⁹ bytes with max input, which overflows 64-bit `size_t`. Wrapped
small value flows into `std::vector<uint8_t>(pixel_bytes)`, then `read_exact`
reads past bounds.

**Impact:** Heap/stack OOB read triggerable by a hostile or buggy recon.

---

#### 5. `recon_forwarder.hpp:394-398` — recon-side pixel_bytes overflow (same pattern)

```cpp
size_t npixels = size_t(ihdr->matrix_size[0]) * ihdr->matrix_size[1] *
                 std::max<uint16_t>(ihdr->matrix_size[2], 1) *
                 std::max<uint16_t>(ihdr->channels, 1);
size_t pixel_bytes = npixels * ISMRMRD::ismrmrd_sizeof_data_type(ihdr->data_type);
```

Mirror of #4 on the scanner-return path.

**Impact:** Same.

---

#### 6. `live_image_store.hpp:105` — recon_group_is_complete premature on uninitialized expected_slices

```cpp
inline bool recon_group_is_complete(const MarshalState& state,
                                    const ReconLatestGroupState& group)
{
    if (state.recon_expected_slices <= 1) return true;
    return group.seen_slices.size() >= state.recon_expected_slices;
}
```

`recon_expected_slices` is default-initialized to 0 (`marshal_state.hpp:105`)
and only set from parsed XML header
(`mrd_tcp_listener.hpp:231-252`). The `<= 1` check treats 0 as "complete".
If the first recon image arrives before the header XML is parsed (race,
reconnect, malformed stream, wrong order), the first slice is published as
a complete multislice result.

**Impact:** Latest image shows a partial stack as final; viz presents
wrong data without error.

---

#### 7. `dump_recorder.cpp:42-50` — queue overflow silently drops records

```cpp
if (queue_.size() >= kMaxQueuedJobs || queued_bytes_ + bytes > kMaxQueuedBytes) {
    dropped_records_.fetch_add(queue_.size() + 1);
    dropped_bytes_.fetch_add(queued_bytes_ + bytes);
    queue_.clear();
    queued_bytes_ = 0;
    if (!drop_logged_.exchange(true)) {
        LOG_WARN("Dump queue full; marking dump incomplete...");
    }
    return; // silent
}
```

`enqueue()` returns `void`. Caller in `mrd_tcp_listener` has no return to
check. Drop is surfaced only at dump close as `dump_complete="false"` HDF5
attribute. No backpressure to scanner.

**Impact:** Silent data loss under overload; no runtime signal to throttle
upstream.

---

### MEDIUM

#### 8. `live_image_recorder.cpp:102` — close_scan can hang forever on worker death

```cpp
auto fut = promise.get_future();
// ... enqueue close job that fulfils promise ...
fut.wait();   // no timeout
```

Called from destructor at `live_image_recorder.cpp:51`. If the writer
thread crashed or exited before processing the close job, the promise is
never fulfilled. `fut.wait()` blocks indefinitely, hanging the destructor.

**Impact:** Shutdown hang; blocks teardown path.

---

#### 9. `marshal_main.cpp:327, 334` — bare `catch (...) {}` in push callbacks

```cpp
} catch (...) {}
```

Two push-message callbacks swallow every exception without logging. Real
transport errors become invisible.

**Impact:** Debugging blind spots; failures masquerade as silence.

---

#### 10. `marshal_main.cpp:263-269` — std::stoi on CLI port args uncaught

```cpp
opt.http_port = static_cast<uint16_t>(std::stoi(argv[++i]));
```

`stoi` throws `std::invalid_argument` or `std::out_of_range` on bad input.
No try/catch around option parsing. Unhandled exception terminates the
process.

**Impact:** Bad CLI input kills the process instead of a clean error
message.

---

#### 11. `latest_image_writer.cpp:102` — attribute_string_len silently truncates at 4 GB

```cpp
parsed.header.attribute_string_len = static_cast<uint32_t>(parsed.attributes.size());
```

`attributes` is a `std::string` whose `size()` is `size_t` (up to 2⁶⁴). Cast
silently wraps if ≥ 2³² bytes.

**Impact:** Theoretical. ISMRMRD attributes are typically ≤ a few MB.
Very low real-world risk but should be defensively bounded.

---

### LOW / NIT

#### 12. `marshal_http.hpp:95` vs `mrd_tcp_listener.hpp:367` — waveform size inconsistent notation

```cpp
// marshal_http.hpp:95
size_t data_bytes = size_t(whdr->number_of_samples) * whdr->channels * sizeof(uint32_t);
// mrd_tcp_listener.hpp:367
size_t data_bytes = size_t(whdr.number_of_samples) * whdr.channels * 4;
```

Numerically equivalent today. If the wire format ever changes, one site may
be missed.

**Impact:** Style / maintenance.

---

#### 13. `.env.demo` vs `docker-compose.demo.yml:123,133` — KSPACE_INTERVAL default mismatch

Compose default is `${KSPACE_INTERVAL:-0.5}` (2 FPS). `.env.demo` sets
`KSPACE_INTERVAL=0.033` (30 FPS). Scripts (`demo-docker.sh`,
`demo-persistent.sh`) always pass `--env-file .env.demo`, so scripted
launches are correct. Manual `docker compose up` without env file →
unexpected 2 FPS.

**Impact:** Only misleading under ad-hoc manual invocation.

---

#### 14. `docker-compose.demo.yml:48` — DUMP_RECON_HOST single-dash expansion

```yaml
- ${DUMP_RECON_HOST-mock-recon}
```

Valid shell/compose expansion: uses default if unset. `:-` would also use
default if empty. Not a bug, just inconsistent with other defaults.

**Impact:** None observed.

---

## Confirmed redundancies

- **`scripts/demo-docker.sh` ↔ `scripts/demo-persistent.sh`** — ~78%
  duplicate code (setup, monitor loop). Merge with `--persistent` flag or
  extract `common_demo.sh`.
- **`scripts/bench_fps.sh`** exists at both
  `/workspaces/cwru_data_marshal/scripts/bench_fps.sh` and
  `/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/scripts/bench_fps.sh`,
  byte-identical. Keep one canonical copy.
- **`docker/Dockerfile.image-streamer` ↔ `docker/Dockerfile.kspace-streamer`**
  — ~17 identical header lines out of 42–43 total (~40% duplicate). Extract
  a shared base or multi-stage build.

---

## FALSE POSITIVES from round 1 — kept for independent re-investigation

Each of these was raised by a round-1 auditor and rejected by an independent
round-2 verification agent with quoted evidence. Listed here so that a second
reviewer (e.g. codex) can confirm or overturn.

### F1. `latest_image_writer.cpp:316-396` — missing `H5Fflush` before close

**Round-1 claim:** H5Fclose can leave data in OS buffers; crash between
close and atomic rename leaves corrupt `latest_image.h5` in production.

**Round-2 rejection:** RAII `H5Fclose` triggers HDF5's internal metadata
sync. Atomic rename targets the already-closed tmp file. Crash during
write leaves an orphan tmp file, not a corrupt production file.
`viz_client` recovers from parse failure by skipping the frame and
retrying on the next poll. Durability beyond "next frame overwrites" was
never a system requirement; adding `H5Fflush` per frame would cost
5–50 ms per frame (major FPS regression). Not a real bug.

### F2. `mrd_sink.cpp:56-64` — HDF5 file handle leak on H5Gcreate2 failure

**Round-1 claim:** If `H5Gcreate2` fails after `H5Fopen` succeeds, file
handle not closed before throwing.

**Round-2 rejection:** Code explicitly calls `H5Fclose(file)` at line 62
before throwing. No leak.

### F3. `recon_forwarder.hpp:62-89` — socket leak on `resolver.resolve` throw

**Round-1 claim:** If resolver throws before `net::connect`, socket is
allocated and leaks.

**Round-2 rejection:** `socket_` is `std::unique_ptr<tcp::socket>`. RAII
destructs on exception unwind regardless of where the throw occurs. No
leak.

### F4. `latest_image_writer.cpp:189` — H5Tinsert error chain can clobber status

**Round-1 claim:** Chain of `status = status < 0 ? status : H5Tinsert(...)`
still executes later H5Tinsert calls after an error.

**Round-2 rejection:** Ternary short-circuits. Once `status < 0`, the
false branch (H5Tinsert call) is not evaluated — so later calls do **not**
execute. No clobber, no wasted call.

### F5. `recon_forwarder.hpp:123-126` — send_config_text size overflow

**Round-1 claim:** `uint32_t len = static_cast<uint32_t>(with_nul.size())`
can wrap.

**Round-2 rejection:** `with_nul` is built from a local `std::string` under
marshal's control, not attacker-controlled wire data. Practical sizes are
KB. Not an exploitable overflow.

### F6. `recon_forwarder.hpp:134-141` — send_header size overflow

**Round-1 claim:** Same pattern on XML header length.

**Round-2 rejection:** Same reasoning as F5. The XML is what marshal
already received and parsed; a 4 GB XML would have failed upstream.

### F7. `mrd_tcp_listener.hpp:298-307` — acquisition traj_bytes / sample_bytes overflow

**Round-1 claim:** `size_t traj_bytes = size_t(ahdr.trajectory_dimensions)
* ahdr.number_of_samples * sizeof(float)` can overflow.

**Round-2 rejection:** `trajectory_dimensions` is `uint8_t` (max 255,
typically ≤ 3); `number_of_samples` is `uint16_t`. Max product ≤
255 × 65535 × 4 ≈ 67 MB, well under `size_t` limit. Cannot wrap on 64-bit.

---

## How to reproduce the audit

1. Inner worktree: `.worktrees/mri_data_marshal` on `perf/latest-bulk-prealloc`
2. Run `scripts/bench_fps.sh` to confirm baseline behavior (no warnings,
   steady FPS at ~40 Hz scanner target)
3. Run unit + integration suite:
   ```
   cmake --build build
   python3 -m unittest tests/integration/test_marshal_integration.py -v
   ```
4. None of the confirmed bugs are triggered by the current happy-path
   tests. They would surface under:
   - graceful shutdown with in-flight HTTP (CRITICAL #1–3)
   - crafted MRD headers (HIGH #4, #5)
   - protocol-order inversion / reconnect race (HIGH #6)
   - sustained overload vs dump queue depth (HIGH #7)

## Recommended next steps (opinion, not a directive)

1. Fix CRITICAL #1–3 together: replace `[&state]` + `detach` with
   `shared_ptr<MarshalState>` + tracked sessions, joined at shutdown.
2. Add explicit bounds checks before every size_t product from wire
   fields (HIGH #4, #5).
3. Gate `recon_group_is_complete` on a "header parsed" validity flag
   (HIGH #6).
4. Add a return value / exception from `dump_recorder::enqueue` so the
   caller can decide to block, drop, or escalate (HIGH #7).
5. Reassess MEDIUM / LOW items after the above land.
