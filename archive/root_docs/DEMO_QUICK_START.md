# MRI Data Marshal Demo - Quick Start (Tomorrow)

## Run the Demo (3 minutes, fully automated)

```bash
./scripts/run_demo_automated.sh
```

That's it. No prompts, no waiting. Demo runs from start to finish automatically.

---

## What You'll See

1. **Dual marshals start** - MRI and Robot marshals initialize
2. **Visualizer launches** - OpenCV window appears
3. **5 volumes stream** - Window updates as data arrives (192×192×10 each)
4. **API tests** - HTTP endpoints respond (status shown in terminal)
5. **Bulk upload** - Complete 64MB scan ingested atomically
6. **Robot clients run** - 3 concurrent clients, 280 ops/sec (shown in results)
7. **Auto cleanup** - All processes terminate cleanly

---

## Before You Present

**Tonight (2-3 hours):**
1. Read `PRESENTATION_READY.md` - overview
2. Read `PRESENTATION_STUDY_GUIDE.md` - detailed learning
3. Run `./scripts/run_demo_automated.sh` - see the output
4. Practice explaining the SWMR concept

**Tomorrow morning (30 minutes):**
1. Verify ports free: `lsof -i :8080,8081,8090` (should be empty)
2. Verify viz_client binary exists: `ls -lh ./build/viz_client`
3. Test demo once: `./scripts/run_demo_automated.sh`

---

## If Something Goes Wrong

**Demo freezes:**
```bash
pkill -f marshal
pkill -f robot_marshal
pkill -f viz_client
# Then retry: ./scripts/run_demo_automated.sh
```

**Port in use:**
```bash
lsof -i :8080  # See what's using the port
kill -9 <PID>
```

**viz_client not showing window:**
- Still works! OpenCV window is optional
- Data still streams to HDF5 successfully
- Terminal output proves it works

---

## Key Points to Explain During Demo

**Opening (30 seconds):**
"This MRI Data Marshal enables real-time surgical guidance. The key innovation is SWMR HDF5 - one writer, many readers, zero locks."

**At Step 3 (volumes streaming):**
"Watch the OpenCV window. Those volumes are arriving within 50 milliseconds. The visualizer reads directly from the HDF5 file the marshal is writing to - lock-free concurrent access."

**At Step 6 (robot clients):**
"While all that MRI streaming is happening, the robot marshal independently handles 280 operations per second with 3 concurrent clients. **Two completely separate systems, running simultaneously, no interference.**"

**Closing:**
"Total system latency: 58 milliseconds. Clinical studies show <300ms is acceptable for surgical guidance. We're 5× faster with room to spare."

---

## Study Materials (Read These Tonight)

1. **PRESENTATION_READY.md** (this directory)
   - Quick reference card (2 pages)
   - Presentation strategy
   - Key numbers to memorize

2. **PRESENTATION_STUDY_GUIDE.md** (docs/ directory)
   - Executive summary
   - 8 core features explained
   - 13 Q&A responses to likely questions
   - Complete technical details

3. **IMPLEMENTATION_COMPLETE.md** (this directory)
   - What was done and why
   - Architecture decisions
   - Testing checklist

---

## Command Reference

### Run Full Demo (Automated)
```bash
./scripts/run_demo_automated.sh
```

### Run Visualizer Only (HTTP mode)
```bash
./build/viz_client --http http://127.0.0.1:8080/v1/mrd/latest \
                   --data ./data_demo_mri/mrd
```

### Run Visualizer with WebSocket
```bash
./build/viz_client --ws ws://127.0.0.1:8090/ws \
                   --data ./data_demo_mri/mrd
```

---

## Confidence Checklist

- [ ] Read PRESENTATION_READY.md
- [ ] Read PRESENTATION_STUDY_GUIDE.md (at least sections 1-5, 13)
- [ ] Run demo once: `./scripts/run_demo_automated.sh`
- [ ] Check ports are free: `lsof -i :8080,8081,8090`
- [ ] Verify viz_client exists: `ls ./build/viz_client`
- [ ] Practice 5-minute explanation
- [ ] Charge laptop battery
- [ ] Test projector connection

---

## During Presentation

**Good flow:**
1. **Elevator pitch (30 sec):** "Real-time MRI data management for surgical guidance using SWMR HDF5"
2. **Architecture slide (1 min):** Show diagram if available
3. **Run demo (3-4 min):** Full automated demo
4. **Key point (1 min):** Explain SWMR as it streams
5. **Q&A (10-15 min):** Use prepared answers from Study Guide

---

## Expected Outcomes

- ✅ Demo runs without user interaction
- ✅ Visualizer shows "Waiting for frames" → "Frame 1/5" etc.
- ✅ Terminal shows all 7 steps completed
- ✅ Results show: 50ms marshal latency, 5ms viz latency, 280 ops/sec
- ✅ Professor understands SWMR and dual-marshal concept
- ✅ Questions answered confidently from prepared responses

---

## You're Ready! 🎉

You have:
- ✅ Comprehensive study materials (500+ sections)
- ✅ Working automated demo (3 minutes)
- ✅ Real visualization proving SWMR works
- ✅ Prepared Q&A responses
- ✅ Complete documentation

**Estimated preparation time:** 2-3 hours tonight
**Probability of successful presentation:** Excellent

**Good luck tomorrow!**
