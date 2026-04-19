# CWRU Data Marshal

A high-performance, dual-marshal architecture for synchronized MRI and robotic data management. Designed for clinical reliability, real-time feedback, and high-throughput imaging data streams.

---

## 🛠️ Storage & Transport

- Scanner and reconstruction traffic use **MRD TCP**; the marshal listens on `--mrd-port` and forwards to recon over the same wire protocol.
- `--dump` selects the persistence mode (mutually exclusive):
  - **Live mode (default, no `--dump`):** writes images **and** waveforms (e.g. ECG) to `live/from_scanner/` and `live/from_reconstruction/`. Publishes the closed `latest_image.h5` snapshot under `live/` and serves `GET /image/latest`.
  - **Dump mode (`--dump`):** archive-only. Writes the full stream — raw acquisitions, images, waveforms, text/config — to `dump/from_scanner/` and `dump/from_reconstruction/`. The live snapshot pipeline is disabled and `GET /image/latest` returns `404`.

---

## 🚀 Getting Started

### 1. Interactive Demo
To see the system in action (including real-time streaming, bio-signals, and the safety bridge), run the interactive demo:
```bash
./scripts/run_demo.sh
```
Robot marshal binaries are expected from an external repo. By default, the demo looks for `../robot_data_marshal_with_catheter_system_components`. If it is missing, the demo will create a git worktree from the `robot_data_marshal_with_catheter_system_components` branch in this repo. You can also set `ROBOT_MARSHAL_DIR` (and optional `ROBOT_MARSHAL_BIN` / `ROBOT_CLIENTS`) before running the demo.

### 2. Build and Test
To configure, build, and run the current test suite:
```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

---

## 📖 Documentation Map

### Guides
- **[Quick Start](../../docs/QUICK_START.md):** Fastest path to build and run the MRI marshal.
- **[Developer Guide](../../docs/DEVELOPER_GUIDE.md):** Development workflow, project layout, and implementation notes.
- **[Manual Terminal Setup](../../docs/MANUAL_TERMINAL_SETUP.md):** Step-by-step manual startup and wiring.
- **[External Client Guide](../../docs/EXTERNAL_CLIENT_GUIDE.md):** How external clients connect and interact.

### Architecture
- **[Architecture](../../docs/ARCHITECTURE.md):** Current MRI marshal design, storage layout, and transport flow.
- **[MRI Marshal Protocol Contract](../../docs/MRI_MARSHAL_PROTOCOL_CONTRACT.md):** Protocol expectations for scanner, marshal, and downstream consumers.
- **[Reconstruction Interface](../../docs/RECONSTRUCTION_INTERFACE.md):** MRI reconstruction integration and handoff contract.

### Performance
- **[API Reference](../../docs/API_REFERENCE.md):** Current HTTP and MRD TCP contract.

---

## 📂 Repository Structure

- `scripts/`: Main entry points for demo and testing.
  - `scripts/benchmarks/`: C++ test client harness used for targeted MRD/manual benchmarking.
  - `scripts/tools/`: Auxiliary helper and dev scripts.
- `src/`: MRI Marshal core implementation (Boost.Asio/Beast).
- `clients/`: Reference clients including the **Coordinator Bridge**, trackers, and streamers.
- `tests/`: Comprehensive C++ test suites (9/9 pass).
- `data/`: Default directory for MRI session logs and artifacts.
- `archive/`: Archived test data and historical documentation (preserved, not needed for current work).
  - `archive/root_docs/`: Previous markdown documentation
  - `archive/docs_backup/`: Previous docs folder contents
  - `archive/test_data/`: Test data and experiment directories

---

## 🛡️ Key Features
- **MRD TCP transport:** Scanner-side clients connect to the marshal over MRD TCP, and recon replies travel back on the same transport.
- **Closed snapshot handoff:** in live mode, `GET /image/latest` points readers at atomically replaced, closed `latest_image.h5` snapshots. In dump mode the endpoint returns `404`.
- **Live vs dump are mutually exclusive:** live mode writes images + waveforms under `live/`; dump mode (`--dump`) writes the full stream (acqs + images + waveforms + text) under `dump/` and disables the live snapshot path.
- **Canonical HDF5 history:** Scanner and reconstruction streams are recorded as standard ISMRMRD per-scan files.
- **Volume support:** Handles **2D multislice** series and **full 3D volumes**.
