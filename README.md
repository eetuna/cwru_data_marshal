# CWRU Data Marshal

A high-performance, dual-marshal architecture for synchronized MRI and robotic data management. Designed for clinical reliability, real-time feedback, and high-throughput imaging data streams.

---

## 🛠️ Operational Modes

The marshal runs in one of two explicit modes to ensure data integrity:

### [Mode A: Live Mode](docs/guides/README_MODE_A.md)
*   **Purpose:** Real-time data streaming and feedback.
*   **Workflow:** Scanner → Marshal → **Live Files** → Clients.
*   **Storage:** Data is written to `./data/mrd`.

### [Mode B: Dumpbox Mode](docs/guides/README_MODE_B.md)
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

### 2. Run All Tests
To verify the entire system (Unit, Integration, and Stress tests):
```bash
./scripts/run_all_tests.sh
```

---

## 📖 Documentation Map

### Core Guides
- **[Setup & Ingest](docs/guides/USAGE_WITH_CLIENTS.md):** How to install and start ingesting data.
- **[Mode A: Live Mode](docs/guides/README_MODE_A.md):** Real-time SWMR streaming documentation.
- **[Mode B: Dumpbox Mode](docs/guides/README_MODE_B.md):** Archival recording and playback documentation.
- **[Troubleshooting](docs/guides/TROUBLESHOOTING.md):** Common issues and solutions.

### Technical Architecture
- **[System Architecture](docs/technical/ARCHITECTURE.md):** High-level design and data flow.
- **[API Reference](docs/technical/API_REFERENCE.md):** REST and WebSocket endpoint specifications.
- **[Branch Comparison](docs/technical/BRANCH_COMPARISON.md):** MRI Marshal (SWMR) vs. Robot Marshal (Lightweight Cache).

### Reports & Performance
- **[Performance Report](docs/reports/PERFORMANCE_REPORT.md):** Throughput and latency benchmarks.
- **[Verification Report](docs/reports/REFACTORING_AND_TESTING_REPORT.md):** Audit of current implementation vs. DataFlow design.
- **[Project Roadmap](docs/ROADMAP_IMPROVEMENTS.md):** Current status and planned enhancements.

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