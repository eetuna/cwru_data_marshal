# MRI Marshal - Endpoint Design Rationale

**Document Purpose:** Explains why `/v1/mrd/ingest` handles IMAGE data defensively instead of redirecting to `/v1/mrd/frame`.

**Last Updated:** 2026-01-29

---

## The Question

**Why does `/v1/mrd/ingest` accept IMAGE data and store it to SWMR (with a warning), instead of redirecting it to `/v1/mrd/frame`?**

---

## Quick Answer

**Defensive programming for user-friendliness.**

The current approach:
- ✅ Works with any HTTP client (even simple ones)
- ✅ Stores data safely (no data loss)
- ✅ Educates users with warning message
- ✅ Single round-trip (fast)
- ✅ Simple implementation

Redirecting would:
- ❌ Require redirect-aware clients
- ❌ Two round-trips (slower)
- ❌ Break simple clients
- ❌ More complex code
- ❌ Confusing user experience

---

## Scenario: Client Accidentally Sends Single IMAGE to `/v1/mrd/ingest`

### Current Implementation: Defensive Handling

```http
POST /v1/mrd/ingest HTTP/1.1
Host: localhost:8080
X-MRD-Stream: cardiac_scan
Content-Type: application/octet-stream
Content-Length: 262484

[Body: ImageHeader (340 bytes) + pixel data (262144 bytes)]
```

**Flow:**
```
1. Marshal receives POST to /v1/mrd/ingest
2. Detect type: ImageHeader detected → MrdDataType::IMAGE
3. Route to IMAGE handler in /ingest
4. Store to SWMR (same as /frame would do)
5. Return success response with helpful warning
```

**Response:**
```http
HTTP/1.1 200 OK
Content-Type: application/json

{
  "path": "/session-data/run_20260129_123456/mrd/cardiac_scan.mrd",
  "frame_index": 42,
  "warning": "Single images should use /v1/mrd/frame for streaming",
  "note": "Use /v1/mrd/ingest for complete HDF5 files"
}
```

**Result:**
- ✅ Data stored successfully
- ✅ User gets helpful guidance
- ✅ Client doesn't break
- ✅ Single round-trip
- ✅ No data loss

**User thinks:**
> "Oops, I used the wrong endpoint, but my data is safe. Let me fix my code to use `/frame` next time."

---

### Hypothetical Alternative: Forward to `/frame`

If we implemented forwarding instead of defensive handling:

```http
POST /v1/mrd/ingest HTTP/1.1
Host: localhost:8080
X-MRD-Stream: cardiac_scan
Content-Type: application/octet-stream
Content-Length: 262484

[Body: ImageHeader (340 bytes) + pixel data (262144 bytes)]
```

#### Option A: HTTP 307 Redirect (External)

**Flow:**
```
1. Marshal receives POST to /v1/mrd/ingest
2. Detect type: IMAGE
3. Return HTTP 307 redirect
4. Client must handle redirect
5. Client makes NEW POST to /v1/mrd/frame
6. Data finally stored
```

**Response:**
```http
HTTP/1.1 307 Temporary Redirect
Location: /v1/mrd/frame
Content-Length: 0
```

**Result:**
- ❌ Requires redirect-aware client
- ❌ Two round-trips (double latency)
- ❌ Simple clients break (curl without -L)
- ❌ Client must re-send entire body

**User experience:**
```bash
# Developer accidentally uses wrong endpoint
curl -X POST http://localhost:8080/v1/mrd/ingest \
  --data-binary @single_image.bin

# Result: Fails!
# HTTP 307 Temporary Redirect

# Must use -L flag:
curl -L -X POST http://localhost:8080/v1/mrd/ingest \
  --data-binary @single_image.bin

# Now works, but sent data TWICE (inefficient)
```

**User thinks:**
> "WTF? The endpoint doesn't work! Why is it broken?"

#### Option B: Internal Forward (Transparent)

**Flow:**
```
1. Marshal receives POST to /v1/mrd/ingest
2. Detect type: IMAGE
3. Internally call /frame handler
4. Store data
5. Return /frame response
```

**Response:**
```http
HTTP/1.1 200 OK
Content-Type: application/json

{
  "path": "/session-data/run_20260129_123456/mrd/cardiac_scan.mrd",
  "frame_index": 42
}
```

**Result:**
- ❌ Client called `/ingest`, got `/frame` response (confusing)
- ❌ Harder to debug (which endpoint actually handled it?)
- ❌ Response format inconsistency
- ❌ Client can't tell what happened

**User thinks:**
> "Wait, I called `/ingest`, why does this look like `/frame` response? Is `/ingest` just an alias? What's the difference?"

---

## Design Comparison Table

| Aspect | Current (Defensive) | Hypothetical (Redirect) |
|--------|---------------------|------------------------|
| **Round-trips** | 1 | 2 (Option A) or 1 (Option B) |
| **Client complexity** | Any HTTP client works | Redirect-aware required (A) |
| **User guidance** | Clear warning in response | HTTP status code only (A) |
| **Data safety** | Always stored | Depends on client handling redirect |
| **Performance** | Fast | Slower (A) |
| **Debugging** | Clear (called /ingest, handled by /ingest) | Confusing (A: redirect, B: which handler?) |
| **Code complexity** | Simple | More complex |
| **User experience** | "Helpful and forgiving" | "Strict and pedantic" |

---

## Real-World Usage Examples

### Example 1: Current Implementation (User-Friendly)

```bash
# Developer learning the API
curl -X POST http://localhost:8080/v1/mrd/ingest \
  -H "X-MRD-Stream: test" \
  --data-binary @single_image.bin

# Response includes helpful guidance:
{
  "path": "/session-data/.../test.mrd",
  "frame_index": 0,
  "warning": "Single images should use /v1/mrd/frame for streaming",
  "note": "Use /v1/mrd/ingest for complete HDF5 files"
}

# Developer outcome:
# ✅ Data is stored safely
# ✅ Learns correct endpoint for next time
# ✅ No frustration or confusion
```

### Example 2: Hypothetical Redirect (User-Hostile)

```bash
# Developer learning the API
curl -X POST http://localhost:8080/v1/mrd/ingest \
  -H "X-MRD-Stream: test" \
  --data-binary @single_image.bin

# Response (HTTP 307):
HTTP/1.1 307 Temporary Redirect
Location: /v1/mrd/frame

# curl doesn't follow POST redirects by default
# Result: FAILS

# Developer must figure out:
# 1. Why it failed
# 2. What 307 means
# 3. How to handle redirects in their client
# 4. Re-implement with redirect handling

# Developer outcome:
# ❌ Frustrated
# ❌ Data not stored (until they fix client)
# ❌ Bad first impression of API
```

---

## Why Defensive Handling is Better

### 1. Simplicity

**Current:**
```
One handler processes IMAGE
Clear flow: receive → detect → store → respond
```

**Forward:**
```
Two handlers involved
Complex flow: receive → detect → redirect/forward → re-process → store → respond
More code paths = more bugs
```

### 2. Client Compatibility

**Current:**
```
Works with ANY HTTP client:
- curl (basic)
- Python requests
- C++ Boost.Beast
- Java HttpClient
- JavaScript fetch
- ...literally any HTTP library
```

**Forward (HTTP 307):**
```
Requires redirect-aware clients:
- curl with -L flag
- Custom redirect handling in code
- Libraries that auto-follow redirects
- Many simple clients will break
```

### 3. Error Communication

**Current:**
```json
{
  "warning": "Single images should use /v1/mrd/frame",
  "note": "Use /v1/mrd/ingest for complete HDF5 files"
}
```
**Clear, actionable message in response body**

**Forward (HTTP 307):**
```
HTTP/1.1 307 Temporary Redirect
Location: /v1/mrd/frame
```
**No explanation, just a status code**

### 4. Performance

**Current:**
```
Client → Marshal: POST /ingest (IMAGE data)
Marshal → Client: 200 OK (stored + warning)

Total: 1 round-trip
```

**Forward (HTTP 307):**
```
Client → Marshal: POST /ingest (IMAGE data)
Marshal → Client: 307 Redirect to /frame
Client → Marshal: POST /frame (IMAGE data - SENT AGAIN!)
Marshal → Client: 200 OK (stored)

Total: 2 round-trips
Data sent twice!
```

### 5. User Experience Philosophy

**Current Approach:**
> "We'll help you even if you make a mistake. Here's what you did, here's what worked, and here's how to do it better next time."

**Redirect Approach:**
> "You're wrong. Go somewhere else. Figure it out yourself."

For research/scientific software where users are often learning:
- **Helpful > Strict**
- **Forgiving > Pedantic**
- **Educational > Punishing**

---

## Historical Context

### Actual Endpoint Usage

Looking at existing clients in the codebase:

**`image_streamer` (clients/image_streamer/image_streamer_main.cpp:212):**
```cpp
http::request req{http::verb::post, "/v1/mrd/frame", 11};
```
✅ Uses `/v1/mrd/frame` for streaming images

**No existing client sends IMAGE to `/v1/mrd/ingest`**

The defensive handling is **NOT** for backward compatibility with existing clients - it's for:
1. **Robustness:** Handle edge cases gracefully (defensive programming)
2. **User-friendliness:** Help users who make mistakes
3. **Future-proofing:** If someone does send IMAGE to `/ingest`, don't fail
4. **API design:** Make the API forgiving and helpful, not strict and punishing

---

## Implementation Details

### Current Code Structure

```cpp
// In /v1/mrd/ingest handler
switch (detected_type) {
    case MrdDataType::HDF5_FILE:
        // Primary purpose: batch upload
        return save_complete_file(body);

    case MrdDataType::IMAGE:
        // Defensive handling: wrong endpoint, but help user anyway
        auto result = state.mrd_sink->append_frame(...);
        result.add_field("warning", "Single images should use /v1/mrd/frame");
        result.add_field("note", "Use /v1/mrd/ingest for complete HDF5 files");
        return result;

    case MrdDataType::ACQUISITION:
        // Future: forward to reconstruction service
        return http_501_not_implemented();

    case MrdDataType::UNKNOWN:
        return http_400_bad_request("Invalid format");
}
```

**Simple, clear, single handler per endpoint**

### Hypothetical Forward Code

```cpp
// In /v1/mrd/ingest handler
switch (detected_type) {
    case MrdDataType::HDF5_FILE:
        return save_complete_file(body);

    case MrdDataType::IMAGE:
        // Option A: HTTP redirect
        return http_307_redirect("/v1/mrd/frame");

        // OR Option B: Internal forward
        return frame_handler(body, headers, stream_id);
        // ^ Which handler actually processed it? Hard to debug!

    case MrdDataType::ACQUISITION:
        return http_501_not_implemented();

    case MrdDataType::UNKNOWN:
        return http_400_bad_request("Invalid format");
}
```

**More complex, harder to debug, worse user experience**

---

## Comparison with HDF5 Redirect

### Why HDF5 CAN Be Redirected from `/frame` → `/ingest`

```
POST /v1/mrd/frame
Body: [HDF5 complete file]

Status: NEW capability (never supported before)
Result: Safe to redirect
Reason: No existing clients depend on this
```

**Internal redirect is fine here because:**
- `/frame` was never meant for complete files
- Clear semantic mismatch
- Client expects batch behavior anyway

### Why IMAGE Should NOT Be Redirected from `/ingest` → `/frame`

```
POST /v1/mrd/ingest
Body: [Single IMAGE]

Status: Edge case (not primary purpose)
Result: Handle defensively
Reason: Redirect adds complexity without benefit
```

**Defensive handling is better here because:**
- Edge case, not common
- Redirect hurts user experience
- Storing works fine (same outcome as `/frame`)

---

## Alternative Approaches Considered and Rejected

### Option 1: Return HTTP 400 "Wrong Endpoint"

```json
{
  "error": "wrong_endpoint",
  "message": "IMAGE data must be sent to /v1/mrd/frame",
  "correct_endpoint": "/v1/mrd/frame"
}
```

**Rejected because:**
- ❌ User loses data
- ❌ Unhelpful (doesn't solve their problem)
- ❌ Frustrating user experience
- ❌ Data could have been stored safely

### Option 2: Silent Redirect (HTTP 307)

```
HTTP/1.1 307 Temporary Redirect
Location: /v1/mrd/frame
```

**Rejected because:**
- ❌ Breaks simple clients
- ❌ Two round-trips
- ❌ Data sent twice
- ❌ No educational value

### Option 3: Internal Forward Without Warning

```json
{
  "path": "/session-data/.../test.mrd",
  "frame_index": 0
}
```

**Rejected because:**
- ❌ User never learns correct endpoint
- ❌ Continues using wrong endpoint
- ❌ No guidance provided

### Option 4: Defensive Handling WITH Warning (CHOSEN ✅)

```json
{
  "path": "/session-data/.../test.mrd",
  "frame_index": 0,
  "warning": "Single images should use /v1/mrd/frame for streaming",
  "note": "Use /v1/mrd/ingest for complete HDF5 files"
}
```

**Chosen because:**
- ✅ Data stored safely (no loss)
- ✅ User gets helpful guidance
- ✅ Works with any client
- ✅ Single round-trip
- ✅ Simple implementation
- ✅ Educational

---

## Summary

**Question:** Why does `/v1/mrd/ingest` handle IMAGE instead of redirecting to `/v1/mrd/frame`?

**Answer:** **User-friendliness and robustness.**

### The Philosophy

**Current approach treats the API as:**
- Helpful and forgiving
- Educational
- Robust and defensive
- User-centric

**Redirecting would treat the API as:**
- Strict and pedantic
- Punishing
- Client-hostile
- Fragile

### The Trade-offs

| Approach | Pros | Cons |
|----------|------|------|
| **Defensive Handling** (current) | Simple, fast, user-friendly, robust | `/ingest` has two code paths |
| **HTTP Redirect** | Semantically "pure" | Slow, breaks clients, hostile UX |
| **Return Error** | Semantically "strict" | Data loss, user frustration |

### The Decision

For a research/scientific API where users are learning and experimenting:

**Helpfulness > Purity**

The small cost of having two code paths in `/ingest` is **far outweighed** by:
- Better user experience
- No data loss
- Educational value
- Broader client compatibility
- Simpler client code

---

## References

- **Endpoint Implementation:** [src/marshal_http.hpp](/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/src/marshal_http.hpp)
- **Type Detection:** [include/mrd_type_detector.hpp](/workspaces/cwru_data_marshal/.worktrees/mri_data_marshal/include/mrd_type_detector.hpp)
- **HTTP Routing Examples:** [HTTP_ROUTING_EXAMPLES.md](HTTP_ROUTING_EXAMPLES.md)
- **Implementation Summary:** [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)
