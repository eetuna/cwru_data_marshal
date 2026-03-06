# CWRU Data Marshal

A high-performance, dual-marshal architecture for synchronized MRI and robotic data management. Designed for clinical reliability, real-time feedback, and high-throughput imaging data streams.

---

## 🛠️ Operational Modes

The marshal runs in one of two explicit modes to ensure data integrity:

### Mode A: Live Mode
*   **Purpose:** Real-time data streaming and feedback.
*   **Workflow:** Scanner → Marshal → **Live Files** → Clients.
*   **Storage:** Data is written to `./data/mrd`.

### Mode B: Dumpbox Mode
*   **Purpose:** High-integrity archival recording and offline playback.
*   **Workflow:** Scanner → Marshal → **Session Folders** → Playback → Live.
*   **Storage:** Data is siloed into `./data/dumpbox/<session>`.

---

## 🚀 Getting Started

### 1. Interactive Demo
To see the system in action (including real-time streaming, bio-signals, and the safety bridge), run the interactive demo:
```bash
./scripts/run_demo.sh
```
Robot marshal binaries are expected from an external repo. By default, the demo looks for `../robot_data_marshal_with_catheter_system_components`. If it is missing, the demo will create a git worktree from the `robot_data_marshal_with_catheter_system_components` branch in this repo. You can also set `ROBOT_MARSHAL_DIR` (and optional `ROBOT_MARSHAL_BIN` / `ROBOT_CLIENTS`) before running the demo.

### 2. Run All Tests
To verify the entire system (Unit, Integration, and Stress tests):
```bash
./scripts/run_all_tests.sh
```

---

## 📖 Documentation Map

### Guides
- **[Usage & API](docs/USAGE_AND_API.md):** Configuration, endpoints, and client integration.
- **[Client API Reference](docs/CLIENT_API_REFERENCE.md):** HTTP endpoint specifications.
- **[Demo Guide](docs/DEMO_GUIDE.md):** How to run the interactive demo.

### Architecture
- **[SWMR & Robot Marshal Overview](docs/SWMR_AND_ROBOT_MARSHAL_OVERVIEW.md):** High-level design and data flow.
- **[Caching Architecture](docs/CACHING_ARCHITECTURE.md):** Write-behind caching and async queue pattern.
- **[HDF5 Locking Notes](docs/HDF5_LOCKING_NOTES.md):** WSL2-specific HDF5 file locking.

### Performance
- **[Improvements & Optimization](docs/IMPROVEMENTS_AND_OPTIMIZATION.md):** Throughput baselines and optimization strategies.
- **[SWMR Stress Analysis](docs/SWMR_CONTINUOUS_BENCH_ANALYSIS.md):** Continuous benchmark results and HDF5 constraints.
- **[Presentation](docs/MRI_DATA_MARSHAL_PRESENTATION.md):** Faculty/researcher overview.

---

## 📂 Repository Structure

- `scripts/`: Main entry points for demo and testing.
  - `scripts/benchmarks/`: Exhaustive performance and chaos tests.
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
- **HDF5 SWMR:** Concurrent read/write for real-time MRI visualization.
- **Volume Support:** Native handling of **2D Multislice** series and **Full 3D Volumes**.
- **Flexible Ingestion:** 
    -   Use **Streaming Mode** (`/v1/mrd/frame`) for low-latency live feedback.
    -   Use **Bulk Mode** (`/v1/mrd/ingest`) for efficient 3D volume archival.
- **Process Isolation:** Decoupled Safety (Robot) and Volume (MRI) marshals.
- **Clinical Readiness:** Standardized `/v1/mrd/` API with millisecond precision and error logging.
- **Software E-Stop:** Active bridge monitoring for automated safety responses.
