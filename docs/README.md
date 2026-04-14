# MRI Data Marshal Documentation

All documentation lives in the umbrella repo's `docs/` directory to avoid duplication.

From this worktree, the canonical docs are at `../../docs/`:

- **[ARCHITECTURE.md](../../docs/ARCHITECTURE.md)** — system diagram, MRD TCP wire protocol, compose topology
- **[API_REFERENCE.md](../../docs/API_REFERENCE.md)** — complete HTTP + MRD TCP API reference
- **[RECONSTRUCTION_INTERFACE.md](../../docs/RECONSTRUCTION_INTERFACE.md)** — MRD TCP recon contract
- **[MRI_MARSHAL_PROTOCOL_CONTRACT.md](../../docs/MRI_MARSHAL_PROTOCOL_CONTRACT.md)** — scanner/recon proxy responsibilities
- **[DEVELOPER_GUIDE.md](../../docs/DEVELOPER_GUIDE.md)** — repo organization, replacing mocks
- **[MANUAL_TERMINAL_SETUP.md](../../docs/MANUAL_TERMINAL_SETUP.md)** — per-terminal run guide
- **[QUICK_START.md](../../docs/QUICK_START.md)** — one-liner start
- **[EXTERNAL_CLIENT_GUIDE.md](../../docs/EXTERNAL_CLIENT_GUIDE.md)** — external client integration

## Quick Start

```bash
# Docker Compose (from umbrella repo root)
docker compose --env-file .env.demo -f docker-compose.demo.yml up

# Or manually (from this worktree)
./build/marshal --http 0.0.0.0:8080 --mrd-port 9100 --dump-dir ./data --dump --recon-host localhost --recon-port 9002
```

## Key Design Points

- **MRD TCP** for scanner ↔ marshal ↔ recon (same wire protocol as python-ismrmrd-server)
- **HTTP** for query/control only (viz, pose, transform, health, dump)
- **Canonical HDF5** via libismrmrd appendAcquisition/appendImage/appendWaveform
- **Canonical latest-image H5** for live viz (atomic rename, faster than SWMR)
- **Fault tolerant** — marshal stays up if recon crashes
- **Bidirectional** — reconstructed images pushed back to scanner on the same MRD TCP socket
