# MRI Data Marshal Demo Guide

How to run and demonstrate the MRI Data Marshal system.

---

## Prerequisites

- **Build tools:** CMake 3.16+, C++17 compiler (GCC 9+ or Clang 10+)
- **Libraries:** HDF5, libismrmrd, OpenCV, cpp-httplib, nlohmann-json, libcurl
- **Display:** X11 server (native Linux, WSLg, or VcXsrv on Windows) for the viz client
- **Python 3:** For mock clients (ecg_client, pose_client, http_tracker)

### Verify Build

```bash
ls build/marshal build/kspace_streamer build/image_streamer build/viz_client
```

If binaries are missing, build with CMake:

```bash
cmake -B build && cmake --build build
```

---

## Quick Start

### Option 1: Docker Compose (Recommended)

From the umbrella repository:

```bash
docker compose -f docker-compose.demo.yml up
```

This starts the marshal, kspace_streamer, mock-recon, and viz-client as containers.

### Option 2: Manual Step-by-Step

Open four terminals:

```bash
# Terminal 1: Start the marshal
./build/marshal --http 0.0.0.0:8080 --dump-dir ./data --recon-url http://localhost:9002

# Terminal 2: Start the mock reconstruction service
python3 docker/mock-recon/mock_recon.py --port 9002 --marshal-url http://localhost:8080

# Terminal 3: Start the viz client
./build/viz_client --http http://localhost:8080

# Terminal 4: Run the kspace streamer (sends a full scan)
./build/kspace_streamer --http http://localhost:8080
```

---

## What Happens

1. **kspace_streamer** sends a complete scan: `POST /header`, `POST /config`, repeated `POST /frame` (acquisitions), then `POST /close`.
2. **Marshal** archives every frame to `data/from_scanner/scan_<ts>.h5` using canonical libismrmrd HDF5 (appendAcquisition). Each frame is forwarded to mock-recon.
3. **mock-recon** receives the data, reconstructs images, and posts them back to the marshal via `POST /image`.
4. **Marshal** archives reconstructed images to `data/from_reconstruction/scan_<ts>.h5` and writes the latest image as a standalone file (`latest_image.bin`).
5. **viz_client** polls `GET /image/latest`, reads the standalone file, and displays it with OpenCV.

---

## Demo with Python Mock Clients

After starting the marshal:

```bash
# Send simulated ECG waveforms via POST /frame (ISMRMRD waveform format)
python3 clients/mocks/ecg_client.py --endpoint http://localhost:8080 --count 100

# Send simulated pose updates via POST /pose
python3 clients/mocks/pose_client.py --endpoint http://localhost:8080 --count 50

# Poll image and pose data via GET /image/latest and GET /pose
python3 clients/mocks/http_tracker.py --endpoint http://localhost:8080
```

---

## Verifying Results

### Check archived data

```bash
# List scanner-side archives
curl http://localhost:8080/dump/scanner

# List reconstruction-side archives
curl http://localhost:8080/dump/recon

# Check latest image
curl http://localhost:8080/image/latest
```

### Inspect HDF5 files (after scan completes)

```bash
ls data/from_scanner/
ls data/from_reconstruction/

# Use h5dump or Python ismrmrd to inspect
python3 -c "
import ismrmrd
dset = ismrmrd.Dataset('data/from_scanner/scan_*.h5', 'dataset', False)
print(f'Acquisitions: {dset.number_of_acquisitions()}')
dset.close()
"
```

---

## Resilience Test (T4)

The most important test: the marshal stays running when recon dies.

1. Start marshal + mock-recon + kspace_streamer as above.
2. While the scan is running, kill mock-recon: `pkill -f mock_recon.py`
3. Verify the marshal is still running: `curl http://localhost:8080/health`
4. Verify frames are still being archived: `curl http://localhost:8080/dump/scanner`
5. The viz client will show a "reconstruction failed" PNG.

---

## Startup Flags

| Flag | Description | Default |
|------|-------------|---------|
| `--http host:port` | HTTP listen address | `0.0.0.0:8080` |
| `--ws-port N` | Optional WebSocket port | unset |
| `--recon-url URL` | Reconstruction service URL | unset (archival-only mode) |
| `--dump-dir PATH` | Root directory for HDF5 archives | required |

---

## Troubleshooting

### X11 Display Issues

If the viz client cannot open a window:

```bash
echo $DISPLAY  # Should be :0 or similar
# WSLg (Windows 11) should work automatically
# VcXsrv (Windows 10): export DISPLAY=$(cat /etc/resolv.conf | grep nameserver | awk '{print $2}'):0.0
```

Skip the viz client and monitor via HTTP instead:

```bash
watch -n1 'curl -s http://localhost:8080/image/latest'
```

### Port Already in Use

```bash
lsof -i :8080
kill <PID>
```

### Cleanup

```bash
pkill marshal; pkill viz_client; pkill kspace_streamer; pkill -f mock_recon.py
rm -rf ./data/from_scanner ./data/from_reconstruction
```
