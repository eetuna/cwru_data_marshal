# MRI Marshal — Code Bug Audit (verified)

**Date:** 2026-04-18
**Scope:** marshal transport/HTTP/storage layer + scripts/compose/Dockerfiles
**Branch audited:** `perf/latest-bulk-prealloc` (tip `1822829`) at `.worktrees/mri_data_marshal`
**Method:** two-pass audit. Round 1 produced 25 candidate findings via three parallel auditors. Round 2 ran a fresh verification agent per finding with no prior context, requiring quoted-code evidence. Only findings verified twice are listed as CONFIRMED. False positives kept at bottom for independent re-investigation by another agent (e.g. codex).

## Codex independent verification pass

**Date:** 2026-04-18
**Reviewer:** Codex
**Result:** mostly confirmed, with corrections below.

- Branch metadata is correct: umbrella `audit/mri-marshal-protocol-fixes-umbrella`
  at `2a89f86`, inner MRI worktree `perf/latest-bulk-prealloc` at `1822829`.
- Findings #2, #3, #5, #6, #9, #10, #11, #12, #13, #14 and the docs
  `DEVELOPER_GUIDE.md` action are supported by the current code/docs.
- Finding #1 is directionally valid as a lifetime/shutdown hazard, but the
  mechanism is overstated. `LatestImageWriter` is owned by `MarshalState`,
  and its destructor drains queued jobs before the `MarshalState` subobjects
  used by the completion lambda are destroyed. The stronger evidence is the
  detached MRD/HTTP session lifetime, plus recon callbacks during shutdown:
  detached threads can still access `MarshalState`, and MRD/recon paths can
  enqueue new latest-image work while `main()` is returning and `state`
  destruction has begun. Also, current HTTP handlers do not call
  `publish_latest_snapshot()`; MRD/recon paths do.
- Finding #4 is a real missing bounds/overflow check, but the stated "read
  past bounds" impact is not precise for the scanner-side path. A wrapped
  `pixel_bytes` value causes an undersized read/allocation and protocol
  desynchronization/DoS risk; additional overflow in aggregate body sizing
  would be needed for a direct OOB write.
- Finding #7 is a confirmed lossy behavior with no caller-visible backpressure
  or return value. It is not fully silent at runtime: the first overflow logs a
  warning and the final HDF5 gets `dump_complete="false"`.
- Finding #8 is not confirmed as written. `LiveImageRecorder` catches
  `std::exception` from jobs, and normal destruction enqueues the close job
  before setting `stopping_`. A hang would require the worker to be stuck in a
  blocking operation, `close_scan()` to be called from the worker thread, or
  another abnormal condition; a worker "death" from an uncaught non-`std`
  exception would normally terminate the process, not leave the destructor
  waiting forever.
- The confirmed redundancies are supported: the two `bench_fps.sh` copies are
  byte-identical, the demo scripts duplicate compose setup/monitoring flow, and
  the streamer Dockerfiles share a large common prefix.

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

---

## Docs audit — 2026-04-18

Same verification-pass method as the code audit above. Round 1: two
parallel auditors (one for umbrella `docs/`, one for inner worktree
docs). Round 2: independent verification agent per flagged doc.

**Scope:** all non-archive docs in umbrella `docs/` and inner worktree
docs/.

### Umbrella `docs/` (15 files)

| Doc                                                        | Status |
|------------------------------------------------------------|--------|
| README.md (repo root)                                      | KEEP   |
| docs/API_REFERENCE.md                                      | KEEP   |
| docs/ARCHITECTURE.md                                       | KEEP   |
| **docs/DEVELOPER_GUIDE.md**                                | **UPDATE** |
| docs/DUMP_QUICK_START.md                                   | KEEP   |
| docs/EXTERNAL_CLIENT_GUIDE.md                              | KEEP   |
| docs/MANUAL_TERMINAL_SETUP.md                              | KEEP   |
| docs/MRI_LATEST_IMAGE_DIAGNOSIS_AND_OPTIONS_2026-04-16.md  | KEEP   |
| docs/MRI_LATEST_IMAGE_EXECUTIVE_SUMMARY_2026-04-16.md      | KEEP   |
| docs/MRI_LATEST_IMAGE_EXPERIMENTS_AUDIT_2026-04-17.md      | KEEP   |
| docs/MRI_MARSHAL_BUG_AUDIT_2026-04-18.md                   | KEEP (this doc) |
| docs/MRI_MARSHAL_PROTOCOL_CONTRACT.md                      | KEEP   |
| docs/MRI_OPTION6_PIPELINE_REFACTOR_PLAN_2026-04-18.md      | KEEP   |
| docs/QUICK_START.md                                        | KEEP   |
| docs/RECONSTRUCTION_INTERFACE.md                           | KEEP   |

### Inner worktree (`.worktrees/mri_data_marshal/`) — 4 files

| Doc                                          | Status |
|----------------------------------------------|--------|
| README.md                                    | KEEP   |
| docs/README.md                               | KEEP   |
| .devcontainer/README.devcontainer.md         | KEEP   |
| clients/mocks/README.md                      | KEEP   |

### Confirmed action: `docs/DEVELOPER_GUIDE.md` update

**Issue:** stale branch architecture. Two inconsistent sections in the
same file.

Lines 9-12 (branch diagram):
```
main                          <- Umbrella branch
├── mri-data-marshal          <- MRI marshal server + clients
└── robot-data-marshal        <- Robot marshal server + clients
```

Lines 24-26 (table, contradicts diagram):
```
| main                       | Docs, Dockerfiles, compose files, scripts |
| audit/live-atomic-rename   | MRI marshal server (C++) ...              |
| robot-data-marshal         | Robot marshal server (C++) ...            |
```

Current git state (verified via `git branch` + `git worktree list`):
- **Active umbrella:** `audit/mri-marshal-protocol-fixes-umbrella`
- **Inner worktree on:** `perf/latest-bulk-prealloc` (was `audit/live-atomic-rename`)
- Experiment branches: `perf/latest-{image-shared-buffers,file-reuse,slot-reuse,bulk-prealloc}`
- `main`, `mri-data-marshal`, `robot-data-marshal` branches still exist but are not the current workflow

**Recommended action:** rewrite lines 8-26 of DEVELOPER_GUIDE.md to reflect
the `audit/*` umbrella pattern + experiment branches in worktrees, and
mark the old `main` / `mri-data-marshal` / `robot-data-marshal` layout as
historical context.

### False positive dropped in verification

- **`.devcontainer/README.devcontainer.md`** was flagged as "stale"
  (80 days old, 9 lines). Verified: content matches
  `.devcontainer/devcontainer.json` exactly (`context="."`,
  `dockerfile="Dockerfile"`). Short + old ≠ stale when content is
  factually correct. No action.

### Cross-document consistency

No contradictions found between protocol/architecture/API docs. No
references to SWMR (correctly absent — last SWMR branch is
`feature/kspace-streamer-real-recon`, tip 2026-04-09). No archive docs
referenced as authoritative by non-archive docs.

---

## Verification of Codex's corrections — 2026-04-18

After Codex added its independent verification pass (top of this doc), a
third round of parallel agents verified each of Codex's 4 corrections
against the actual code. Results:

| Codex correction | Verdict after re-verification |
|------------------|-------------------------------|
| #1 (lambda UAF mechanism) | **MOSTLY RIGHT** — Codex's 3 sub-claims: (a) LatestImageWriter destructor drains jobs first → RIGHT, (b) real hazard is detached session threads enqueuing during teardown → RIGHT, (c) "HTTP handlers do NOT call `publish_latest_snapshot`" → **WRONG**. HTTP's `handle_recon_image` → `append_live_image(state, Recon, ...)` → `publish_latest_snapshot`. Both HTTP and MRD paths reach it. |
| #2 (pixel_bytes not OOB write) | **RIGHT** — verified both `mrd_tcp_listener.hpp:335-341` and `recon_forwarder.hpp:394-406`. `std::vector<uint8_t>(pixel_bytes)` + `read_exact(..., pixel_bytes)` uses the wrapped-small value for both alloc and read count. No OOB write. Impact is undersized allocation + protocol desync/DoS. |
| #3 (dump not silent) | **RIGHT** — first overflow logs via `LOG_WARN` guarded by `drop_logged_.exchange(true)` (`dump_recorder.cpp:47-48`). Close path writes `dump_complete="false"` to HDF5 (`dump_recorder.cpp:142`). Silent only to caller API (void return). |
| #4 (close_scan hang) | **RIGHT** — worker catches `std::exception`; non-std exception triggers `std::terminate` (process crash, not destructor hang). Destructor enqueues close job before setting `stopping_` so happy path always fulfils the promise. No realistic hang path. **Finding #8 dropped.** |

### Net effect of all verification passes

Round-1 candidate findings: **25**. After Round-2 verification (6 parallel
agents, each claim independently evidenced): **14 CONFIRMED**. After
Codex's correction pass + Round-3 re-verification of Codex: **13
CONFIRMED**.

**Changes from this third pass:**

- **Finding #1** — mechanism reworded: the UAF is not about in-flight
  jobs seeing a destroyed state. `LatestImageWriter`'s destructor drains
  queued jobs before the sub-members the lambda touches are destroyed.
  The real hazard is **detached MRD and HTTP session threads enqueuing
  NEW work after `main()` has begun tearing down `MarshalState`**. Same
  class of UAF, different mechanism.
- **Finding #4** / **#5** — impact downgraded from "OOB read past bounds"
  to "undersized allocation → protocol desynchronization → DoS". Still
  HIGH (attacker-influenced wire fields → service disruption), no direct
  memory-corruption write.
- **Finding #7** — wording changed from "silent drop" to "dropped without
  caller-visible backpressure; warns once on first overflow; HDF5 records
  `dump_complete="false"` on close".
- **Finding #8** — **dropped as false positive.** `close_scan()` has no
  realistic hang path given the existing exception handling and
  enqueue-before-stopping semantics.
- Findings #2, #3, #6, #9, #10, #11, #12, #13, #14 — no change.

### Final bug count — 13 CONFIRMED

- 3 CRITICAL (#1 reworded, #2, #3)
- 4 HIGH (#4, #5 impact downgraded; #6, #7 reworded)
- 3 MEDIUM (#9, #10, #11 — #8 dropped)
- 3 LOW/NIT (#12, #13, #14)

Plus 3 confirmed redundancies (demo scripts, bench_fps duplicate, streamer
Dockerfile prefix).

### Notes on Codex's accuracy

Codex reviewed the audit's 14 findings and challenged 4 of them. Three
challenges held on re-verification; one challenge (a sub-claim within
correction #1) was itself wrong. Net: Codex correctly refined 3.75 of 4
items. Its pass added real signal — without it, Findings #1, #4, #5, #7
would remain stated in stronger-than-warranted terms, and Finding #8
would still be listed as a real bug.
