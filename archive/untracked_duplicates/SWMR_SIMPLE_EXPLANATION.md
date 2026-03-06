# SWMR During Reconstruction - Simple Explanation

**Last Updated:** 2026-01-29

---

## The Question

**Can visualization clients read from SWMR while the marshal is getting data back from the reconstruction service?**

**Answer: YES, absolutely.**

---

## Why It Works

The marshal spends **99.9% of its time NOT writing to the SWMR file**.

### What the Marshal Does

```
Step 1: Receive raw k-space from scanner           (1ms)    - NOT writing to SWMR
Step 2: Send to reconstruction service             (1ms)    - NOT writing to SWMR
Step 3: WAIT for reconstruction to finish          (5000ms) - NOT writing to SWMR ⭐
Step 4: Receive reconstructed image back           (1ms)    - NOT writing to SWMR
Step 5: Parse the response                         (1ms)    - NOT writing to SWMR
Step 6: Write to SWMR file                         (5ms)    - WRITING ⚠️
Step 7: Send response to scanner                   (1ms)    - NOT writing to SWMR

Total time: 5010ms
Time writing to SWMR: 5ms (0.1%)
Time NOT writing to SWMR: 5005ms (99.9%)
```

**During Step 3 (the long wait):** Viz clients can read the SWMR file with ZERO interference.

---

## Simple Timeline

```
What Marshal Is Doing                 Can Viz Clients Read SWMR?
──────────────────────────────────    ───────────────────────────

t=0s    Receive raw k-space           ✅ YES
t=0s    POST to recon service         ✅ YES
t=0s    Waiting...                    ✅ YES
t=1s    Still waiting...              ✅ YES
t=2s    Still waiting...              ✅ YES
t=3s    Still waiting...              ✅ YES
t=4s    Still waiting...              ✅ YES
t=5s    Got response back!            ✅ YES
t=5s    Parsing response              ✅ YES
t=5s    Writing to SWMR (5ms)         ⏸️  Brief pause (only if reading same frame)
t=5s    Write done                    ✅ YES
t=5s    Sending response              ✅ YES
```

---

## What "Brief Pause" Means

When the marshal writes to SWMR for 5ms:

**Viz clients reading OLD frames:** ✅ No pause at all
**Viz clients reading the NEW frame being written:** ⏸️ Wait 5ms

### Example

```
Marshal is writing frame 100 to SWMR:

Viz Client A reading frame 99:  ✅ Reads immediately (no wait)
Viz Client B reading frame 98:  ✅ Reads immediately (no wait)
Viz Client C reading frame 100: ⏸️  Waits 5ms for write to finish
```

---

## Real Numbers

### One Frame with Reconstruction

```
Total time: 5 seconds
Marshal writing to SWMR: 0.005 seconds (5ms)
Marshal NOT writing: 4.995 seconds

During those 4.995 seconds, viz clients can read freely.
```

### 100 Frames Per Second

If the marshal processes 100 frames/second:
- Each frame takes 10ms
- SWMR write takes 5ms
- Viz clients can read freely for 5ms out of every 10ms
- **50% of the time is lock-free**

With reconstruction (5 seconds per frame):
- Each frame takes 5000ms
- SWMR write takes 5ms
- Viz clients can read freely for 4995ms out of every 5000ms
- **99.9% of the time is lock-free**

---

## The Bottom Line

**Yes, viz clients can read while reconstruction is happening.**

The only tiny moment they might wait is during the final 5ms write to SWMR, and even then:
- Only if reading the exact frame being written
- Only for 5ms (you won't even notice)

---

## Simple Flow Diagram

```
Scanner          Marshal          Recon Service          SWMR File          Viz Clients
   │                │                    │                    │                   │
   │ raw k-space    │                    │                    │                   │
   ├───────────────>│                    │                    │                   │
   │                │ POST /reconstruct  │                    │                   │
   │                ├───────────────────>│                    │                   │
   │                │                    │                    │  reading frame 99 │
   │                │ [waiting 5s...]    │ [processing...]    │<──────────────────┤
   │                │                    │                    │                   │
   │                │                    │                    │  reading frame 98 │
   │                │                    │                    │<──────────────────┤
   │                │                    │                    │                   │
   │                │ HTTP 200 OK        │                    │                   │
   │                │<───────────────────┤                    │                   │
   │                │                    │                    │                   │
   │                │ [parse 1ms]        │                    │                   │
   │                │                    │                    │                   │
   │                │        write frame 100 (5ms)            │                   │
   │                ├───────────────────────────────────────>│                   │
   │                │                    │                    │                   │
   │ HTTP 201       │                    │                    │  reading frame 100│
   │<───────────────┤                    │                    │<──────────────────┤
   │                │                    │                    │                   │
```

**Key point:** While marshal waits for reconstruction (5 seconds), viz clients are reading freely.

---

## FAQ

**Q: Will viz clients see delays?**
A: Only 5ms when reading the exact frame being written. Otherwise, no delay.

**Q: Can multiple viz clients read at the same time?**
A: Yes! SWMR = Single Writer, Multiple Readers. That's the whole point.

**Q: What if reconstruction fails?**
A: Marshal never writes to SWMR. Viz clients are unaffected.

**Q: Does the marshal block while waiting for reconstruction?**
A: Yes, but it's not writing to SWMR during that time, so viz clients can read freely.

---

## Summary

✅ Viz clients can read SWMR during reconstruction
✅ 99.9% of the time there's zero contention
✅ Brief 5ms pause only when reading the exact frame being written
✅ SWMR is designed exactly for this use case

**It just works.**
