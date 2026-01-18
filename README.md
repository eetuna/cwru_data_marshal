# CWRU Data Marshal (Umbrella Runner)

This branch is an **umbrella** that runs the MRI marshal and robot marshal from their dedicated branches/repos. It does not vendor marshal source code; it orchestrates builds, demos, and Docker packaging.

---

## 🚀 Getting Started

### 1. Interactive Demo (umbrella)
To see the system in action (real-time streaming, bio-signals, and the safety bridge), run the interactive demo:
```bash
./scripts/run_demo.sh
```
The demo uses worktrees for both marshals:
- MRI marshal: `mri-data-marhsal` branch (worktree created at `../mri_data_marshal_worktree`)
- Robot marshal: `robot_data_marshal_with_catheter_system_components` branch (worktree created at `../robot_data_marshal_catheter_worktree`)

You can override with `MRI_MARSHAL_DIR` and `ROBOT_MARSHAL_DIR` if you keep the repos elsewhere.

### 2. Docker Compose (umbrella)
Build and run both marshals as containers:
```bash
docker compose up --build
```

To export images for offline use (USB transfer):
```bash
./scripts/export_images.sh
```

---

## 📖 Documentation Map

### Core Guides
- **[Usage & API](docs/USAGE_AND_API.md):** How to ingest data and use endpoints.
- **[Client API Reference](docs/CLIENT_API_REFERENCE.md):** Client request/response schema details.
- **[Demo Guide](docs/DEMO_GUIDE.md):** Walkthrough of the demo flows.

### Technical Architecture
- **[SWMR vs Robot Marshal](docs/SWMR_AND_ROBOT_MARSHAL_OVERVIEW.md):** MRI SWMR vs robot cache overview.

### Reports & Performance
- **[SWMR Bench Analysis](docs/SWMR_CONTINUOUS_BENCH_ANALYSIS.md):** SWMR throughput/latency analysis.
- **[Improvements & Optimization](docs/IMPROVEMENTS_AND_OPTIMIZATION.md):** Design notes and optimization ideas.

---

## 📂 Repository Structure

- `scripts/`: Umbrella entry points for demos and packaging.
- `scripts/tools/`: Worktree helpers for MRI/robot marshals.
- `docker/`: Dockerfiles that build images from the upstream branches.
- `docs/`: Documentation and handoffs (historical and current).

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
