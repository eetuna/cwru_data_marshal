# MRI Data Marshal Documentation

## Documentation Index

- **[DEMO_GUIDE.md](DEMO_GUIDE.md)** -- How to run the demo (Docker Compose or manual)
- **[USAGE_AND_API.md](USAGE_AND_API.md)** -- Usage instructions, startup flags, and full API reference
- **[CLIENT_API_REFERENCE.md](CLIENT_API_REFERENCE.md)** -- HTTP endpoint specs with curl and Python examples

## Quick Start

```bash
# Docker Compose (from umbrella repo)
docker compose -f docker-compose.demo.yml up

# Or manually
./build/marshal --http 0.0.0.0:8080 --dump-dir ./data --recon-url http://localhost:9002
```

See [DEMO_GUIDE.md](DEMO_GUIDE.md) for the full walkthrough.

## System Components

1. **Marshal** -- HTTP server that archives scanner data, forwards to recon, and serves images for visualization
2. **kspace_streamer** -- C++ scanner mock (sends acquisitions via /header, /config, /frame, /close)
3. **image_streamer** -- C++ image mock (sends synthetic ISMRMRD images)
4. **viz_client** -- C++ visualization client (polls /image/latest, reads standalone file, displays with OpenCV)
5. **mock-recon** -- Python reconstruction mock (receives forwarded data, posts images back)
6. **ecg_client** -- Python ECG waveform producer (sends ISMRMRD waveforms via /frame)
7. **pose_client** -- Python pose updater (POST /pose)
8. **http_tracker** -- Python poller (GET /image/latest, GET /pose)

## Architecture

```
Scanner/Mock ──POST /header,/config,/frame,/close──> Marshal ──forward──> Recon
                                                       |                    |
                                                       |  <──POST /image────┘
                                                       |
                                                       ├── from_scanner/*.h5
                                                       ├── from_reconstruction/*.h5
                                                       └── latest_image.bin
                                                              ↑
                                                        viz_client polls
                                                        GET /image/latest
```

## Key Design Points

- **Canonical HDF5:** All archives use libismrmrd's appendAcquisition/appendImage/appendWaveform.
- **Standalone file for live viz:** The latest reconstructed image is written as a raw binary file (atomic rename). The viz client reads it from disk -- no network image delivery.
- **Resilient:** If recon goes down, the marshal keeps archiving scanner data and shows a failure PNG to the viz client.
- **Transparent proxy:** Recon receives byte-for-byte identical data regardless of source.
- **Two dump directories:** `from_scanner/` and `from_reconstruction/` keep scanner and recon data separate.
