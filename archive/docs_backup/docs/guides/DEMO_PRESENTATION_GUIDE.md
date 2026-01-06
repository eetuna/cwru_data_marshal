# CWRU Data Marshal: Comprehensive Demo & Presentation Guide

This guide provides a structured narrative and technical script for demonstrating the CWRU Data Marshal system, based on the `DataFlow.drawio` architecture.

---

## 1. The Presentation Context (The "Why")
**Objective:** Explain the necessity of the Dual-Marshal architecture.

*   **The Problem:** MRI scanners generate high-bandwidth image data ("The Firehose"). Robots require simple state synchronization ("The State Board"). A single server handling both risks complexity—mixing high-throughput streaming with lightweight state updates increases code complexity and testing burden.
*   **The Solution:**
    *   **MRI Data Marshal (Current Branch):** The hardened, SWMR-enabled hub designed for high-throughput persistent journaling of MRI images, Bio-signals, and session tracking.
    *   **Robot Data Marshal (Branch: robot-data-marshal):** A lightweight generic server with RAM-based caching for simple state synchronization.
*   **Logical Decoupling:** This separation ensures that an MRI reconstruction crash or network bottleneck cannot physically affect the robot's ability to receive a safety HALT command.

---

## 2. Capability Showcase (What we are presenting)
1.  **Hardened Ingestion:** Standardized `/v1/mrd/` API for both frames and full files.
2.  **Biological Signal Hub:** New real-time waveform ingestion (`/v1/bio/signal`) with strict topic isolation.
3.  **SWMR Performance:** Demonstrating sub-150ms end-to-end latency for 3D volumes.
4.  **Security & Reliability:** Integrated race-condition protection and path sanitization.
5.  **Software E-Stop:** An external "brain" (Coordinator) that bridges the decoupled marshals for safety.

---

## 3. Step-by-Step Demo Script (Instructions)

### Setup: Environment Preparation
```bash
# 1. Clear previous demo data
rm -rf ./data_demo_mri ./data_demo_robot ./data_demo_dumpbox
mkdir -p ./data_demo_mri ./data_demo_robot ./data_demo_dumpbox

# 2. Build the MRI Marshal (From this hardened branch)
cmake --build build

# 3. Import the specialized Robot Marshal code from the other branch
# (Handled automatically by the run_demo.sh script)
```

### Step 1: Start the Dual-Marshal Servers
We run two **isolated processes** to prove they are redundant.
```bash
# Terminal command:
./scripts/run_demo.sh
```

---

## 4. Performance & Reliability Data
Reference the generated reports for deep technical questions:
*   **`docs/reports/PERFORMANCE_REPORT.md`:** Shows peak 43 MB/s throughput.
*   **`docs/reports/REFACTORING_AND_TESTING_REPORT.md`:** Proves 100% test pass rate across 9 suites.

---

## 5. Potential Q&A

**Q: Is the MRI Marshal code stable?**
**A:** Yes. We performed a "Chaos Test" bombarding the server with mixed Pose, Bio, and Bulk traffic simultaneously without a single drop or crash.

**Q: Why not merge the branches?**
**A:** To keep the "Safety Loop" simple. The Robot Marshal is less than 500 lines of code—easy to audit for medical safety. The MRI Marshal is complex and feature-rich. Separating them is a feature, not a bug.
