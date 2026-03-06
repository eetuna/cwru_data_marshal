# HANDOVER: Robot Clients Stuck - 4 of 5 Clients Not Incrementing

**Date:** 2026-01-26
**Priority:** HIGH
**Branch:** `robot_data_marshal_with_catheter_system_components`

---

## Symptom

Robot clients show these counters when running:
```
[18:49:31] Robot Clients: cath=3513 ctrl=2900 plan=2376 fe=2651 surf=59123
```

**Only `surf` (surface tracking) is increasing.**
**The other 4 clients are STUCK** at fixed values:
- `cath` (catheter tracking) = 3513 (frozen)
- `ctrl` (controller) = 2900 (frozen)
- `plan` (planning) = 2376 (frozen)
- `fe` (front-end) = 2651 (frozen)

---

## What Was Working Before

User confirmed: "it was working completely fine before"

All 5 clients were incrementing their counters normally.

---

## Current Status

### Working:
- `surf` reads `streaming_2D_images` - **WORKS** because image-streamer is writing MRI frames

### Broken:
- `cath` reads `/read/tip_position_orientation` - **STUCK**
- `ctrl` reads `/read/desired_planned_motion` - **STUCK**
- `plan` reads/writes `/read/desired_planned_motion` + `/write/desired_planned_motion` - **STUCK**
- `fe` reads `/read/user_input` - **STUCK**

---

## Test Result

Manual test shows error:
```bash
curl http://localhost:8081/read/tip_position_orientation
{"error":"File not found"}
```

Robot marshal returns "File not found" for those channels.

---

## Key Evidence

1. **Only surf is working** - indicates image data path is fine
2. **4 clients stuck at same counts** - they hit errors and stopped
3. **Robot marshal returns "File not found"** - channel files don't exist
4. **User says it worked before** - something changed/broke

---

## Investigation Needed

1. **Check robot marshal logs** for errors/startup issues
2. **Verify channel files exist** - where are they supposed to be created?
3. **Check if robot clients are supposed to create initial files** or marshal creates them
4. **Compare current robot marshal code** to last known working version
5. **Check docker-compose.demo.yml** - any volume mount issues?

---

## Files to Check

| Location | What to Look For |
|----------|------------------|
| Robot marshal logs | Startup errors, file creation failures |
| `.worktrees/robot_data_marshal/` | Robot marshal source code |
| `docker-compose.demo.yml` | Volume mounts for robot marshal |
| Robot client source | Error handling, retry logic |

---

## Commands to Run

```bash
# Check robot marshal logs
docker compose -f docker-compose.demo.yml logs robot-marshal | grep -i error

# Check what files robot marshal creates on startup
docker compose -f docker-compose.demo.yml exec robot-marshal ls -la /data/

# Check robot worktree branch
cd .worktrees/robot_data_marshal
git log --oneline -5
git status

# Find robot client source code
find .worktrees/robot_data_marshal -name "*client*" -type f
```

---

## Expected Behavior

All 5 robot clients should increment counters continuously:
```
[18:49:31] Robot Clients: cath=3520 ctrl=2910 plan=2385 fe=2660 surf=59150
[18:49:33] Robot Clients: cath=3530 ctrl=2920 plan=2395 fe=2670 surf=59175
```

---

## Current Context

- **MRI side**: Working fine, just added bio/pose cache (unrelated to robot issue)
- **Robot side**: Separate branch, file-based channel communication
- **Only surf works**: Image data is flowing, other channels are not

---

## Next Steps

1. Find the root cause of "File not found" errors
2. Determine why channel files aren't being created
3. Fix robot marshal or client initialization
4. Verify all 5 clients increment after fix
