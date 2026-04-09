# viz_client shows the last image forever after a stream stops

Status: **accepted / not fixing**. This doc records the behavior, why it
happens, every option considered, and why each was rejected, so the next
person doesn't have to re-derive it.

## Observed behavior

1. Start marshal, recon-sim (or image_streamer), and viz_client.
2. Scanner / streamer pushes frames. viz_client renders them live — good.
3. Stop the scanner / streamer. viz_client keeps displaying the last frame
   it rendered, indefinitely.
4. Close viz_client and reopen it against the same (still running) marshal.
   viz_client **does not** show "Waiting for data..." — it immediately shows
   the stale last frame from the previous run.

Expected / desired: after both the producer and viz_client have closed, a
newly opened viz_client should be blank ("Waiting for data...") until a
genuinely new frame arrives.

## Why it happens (server side)

Marshal caches the most recently stored frame in memory so
`GET /v1/mrd/latest` can answer in O(1). See:

- [src/marshal_state.hpp:99](../src/marshal_state.hpp#L99) —
  `std::string latest_mrd_json;` + `latest_mrd_mutex`.
- [src/mrd_sink.cpp:614-618](../src/mrd_sink.cpp#L614-L618) — writer updates
  `latest_mrd_json` each time `append_frame` succeeds.
- [src/marshal_http.hpp:1152-1170](../src/marshal_http.hpp#L1152-L1170) —
  GET handler returns the cached JSON as-is; 204 only when the cache is
  empty (first-ever request before any frame has been stored).

The cache is **sticky for the lifetime of the marshal process**. There is
no expiry, no "writer disconnected" signal, no session concept. As long as
marshal keeps running, the last frame it ever saw remains the answer to
`/v1/mrd/latest` forever.

## Why it happens (client side)

viz_client polls `/v1/mrd/latest` in a worker thread, reads the returned
`path` and `frame_index`, opens the HDF5 file, reads the slice, and
`cv::imshow`s it. See:

- [clients/viz_client/viz_client_main.cpp:290-335](../clients/viz_client/viz_client_main.cpp#L290-L335).
- The else-branch for "no data yet" ([:489-496](../clients/viz_client/viz_client_main.cpp#L489-L496))
  only fires when `current_slice_data` is **empty**, which is true at
  startup before the first successful fetch. Once any data latches in, the
  client keeps rendering it — subsequent 204 responses or stale repeats do
  not clear the buffer.

Put another way: neither the server nor the client has any notion of
"this frame is old." The design is "latest = last thing marshal stored
during this process's lifetime."

## Fundamental reason this can't be solved purely server-side

To make "reopened viz_client is blank" work without touching the client,
the server would have to distinguish:

- "a new viz_client just started and wants only fresh frames"
- "a viz_client that has been polling continuously expects to keep seeing
  the latest"

Both look identical on the wire: both are anonymous `GET /v1/mrd/latest`
requests. There is no handshake, no session cookie, no `since` parameter,
and no way to know "when did *this particular caller* start." The only
server-only heuristic is a timeout (clear cache if older than N seconds),
which was explicitly rejected.

## Options considered and why each was rejected

### Option 1 — server expires `/v1/mrd/latest` on age

Marshal tracks `last_update_time` next to `latest_mrd_json`. The GET
handler returns 204 when `now - last_update_time > threshold`.

**Rejected because:**

- Introduces a hardcoded or configurable timeout; the user explicitly did
  not want hardcoded session lifetimes.
- Breaks the contract with `clients/bridge/coordinator.py` (the MRI safety
  bridge, [coordinator.py:29-53](../clients/bridge/coordinator.py#L29-L53)),
  which polls `/v1/mrd/latest` at 20 Hz to watch for fault envelopes. If
  the safety poller starts receiving 204s during silences, the safety
  state machine freezes. The bridge's "error cleared" detection only fires
  when it reads a 200 with no `error` key — with expiry in place, a silent
  period between fault and reset would leave the robot halted. This
  endpoint is **overloaded** for safety; changing its semantics is risky.
- Would require updating existing tests
  ([tests/test_http_handlers.cpp:166-187](../tests/test_http_handlers.cpp#L166-L187))
  which set `latest_mrd_json` directly and don't update any timestamp.

### Option 2 — add a new endpoint `GET /v1/mrd/latest-fresh?max_age_ms=N`

Leaves `/v1/mrd/latest` untouched, adds a parallel endpoint with an
age-bounded response. Coordinator stays on the old endpoint, viz or any
new consumer opts in to the fresh one.

**Technical details:** ~15 lines additive. One new `steady_clock::time_point`
field in `MarshalState`, one line in `mrd_sink.cpp` to stamp it on write,
one new route handler branch in `marshal_http.hpp` that reads the query
param and returns 204 if stale. No new mutex (reuses `latest_mrd_mutex`),
no new thread, no change to the writer hot path beyond one atomic store
inside an already-held lock. Zero impact on dropped frames, HDF5 writes,
`/v1/mrd/since`, SWMR, or any existing consumer.

**Rejected because:**

- viz_client is treated as an opaque prebuilt client that the user cannot
  modify in their deployment. Adding a new server endpoint only helps if
  the client is pointed at it, which it cannot be.

### Option 3 — background sweeper thread that clears `latest_mrd_json`

Marshal runs a tiny periodic task: every 500 ms, if
`now - last_update_time > N`, set `latest_mrd_json = ""`. Next GET returns
204, and viz_client's existing empty-data path renders "Waiting for
data...".

**Technical details:** one new field, one new thread (or piggyback on an
existing one), ~20 lines. Because it actually clears the cache rather than
changing the GET semantics, the behavior is uniform — every consumer of
`/v1/mrd/latest` sees the same "stale means empty" contract. The
coordinator's safety poller survives this (it ignores non-200 responses
and only acts on what it reads), though it is no longer able to distinguish
"silence" from "marshal just started" by reading latest — but it was never
designed to rely on that distinction (it reacts to `data.error` when
present, otherwise does nothing).

**Rejected because:**

- Still requires a hardcoded or configurable timeout, which the user does
  not want.
- Touches marshal, which the user wants to keep untouched and always
  active.
- While the coordinator's safety poller is technically tolerant, changing
  the stickiness of `/v1/mrd/latest` is an uncomfortably load-bearing
  change for any other consumer that might rely on the current semantics.

### Option 4 — client-side "no new frame_index for N seconds → blank"

Track time since `frame_index` last changed in viz_client; when it exceeds
a threshold, clear `current_slice_data` and fall into the existing
waiting-screen path.

**Rejected because:**

- Requires modifying viz_client. Not allowed (treated as opaque).
- Also would need a hardcoded timeout.

### Option 5 — explicit signal from the producer to clear

E.g. on scanner shutdown, send `DELETE /v1/mrd/latest` or a "session end"
marker. Marshal clears the cache on receipt.

**Rejected because:**

- Touches marshal (new endpoint or new handler).
- Requires cooperation from the producer, which in production is a real
  scanner and cannot be modified to emit "session end."

### Option 6 — restart marshal between sessions

Kill and restart the marshal process; the cache is process-local so a new
marshal has an empty cache and viz blanks until the next real frame.

**Rejected because:**

- User wants marshal to stay up always. That's the whole point of the
  persistent hub model.

## Why it is ambiguous in principle

The request is "detect session boundaries without any signal that tells
the server a session ended, without a timeout, without client changes,
without producer changes, without marshal changes." Session boundaries
are not observable under those constraints. Any solution must relax
exactly one of them. This doc exists so nobody tries again.

## Current state: accepted

Behavior stays as-is. viz_client displays the last stored frame
indefinitely while marshal is running. Operators who want a visually
"blank" viz on reopen have two practical workarounds:

1. Restart marshal when starting a new experiment.
2. Use a different stream name for each session so the cached frame from
   a previous session is from a different `X-MRD-Stream` — and run
   viz_client against that stream. (Note: `/v1/mrd/latest` is not
   stream-scoped; it returns the most recent frame regardless of stream.
   This workaround only helps visually if the new session actually posts
   a frame soon after viz_client connects, which masks the old one.)

Neither workaround is a "fix" — they are operational habits. The design
decision is that `/v1/mrd/latest` is a last-known-state surface, not a
session-scoped live feed.

## Relevant code references

- Cache storage: [src/marshal_state.hpp:99](../src/marshal_state.hpp#L99)
- Cache write: [src/mrd_sink.cpp:614-618](../src/mrd_sink.cpp#L614-L618)
- Cache read (GET handler): [src/marshal_http.hpp:1152-1170](../src/marshal_http.hpp#L1152-L1170)
- viz_client fetch + render: [clients/viz_client/viz_client_main.cpp:290-496](../clients/viz_client/viz_client_main.cpp#L290-L496)
- Safety poller (depends on current sticky semantics): [clients/bridge/coordinator.py:29-53](../clients/bridge/coordinator.py#L29-L53)
- Handler tests that write the cache directly: [tests/test_http_handlers.cpp:166-187](../tests/test_http_handlers.cpp#L166-L187)
