# CRM_CPPImplementation

C++ implementation of the **Cosserat Rod Model (CRM)** kinematic model for MRI-guided robotic cardiac catheters. The library computes forward kinematics and Jacobians for catheters with multiple flexible and rigid segments, magnetic actuator coil sets, and optional tip-contact constraints.

> **Tip-embedded-in-tissue behaviour is not included.** The two supported contact modes are free tip and fixed-tip point contact.

---

## Repository Layout

```
CatheterProject/
├── eigen5/                         ← Eigen header-only library (place here, see below)
└── CRM_CPPImplementation/
    ├── src/                        ← Library headers and source files
    │   ├── CRM.hpp                 ← Primary public API
    │   ├── CRM_BVPSolver.cpp
    │   ├── CRM_CatheterClass.cpp
    │   ├── CRM_ForwardKinematics.cpp
    │   ├── CRM_IVPSolver.cpp
    │   ├── CRM_IVPJacobian.cpp
    │   └── CRM_SupportFunctions.cpp
    ├── numerical/                  ← MINPACK Trust-Region Dogleg NL solver (C++ port)
    ├── CRMCPPLib/                  ← Visual Studio static library project
    ├── CRMCPPTest/                 ← Visual Studio console test application project
    ├── matlab/                     ← MATLAB MEX wrappers and examples
    ├── python/                     ← Python (nanobind) bindings and examples
    ├── catheterdata/               ← Sample catheter parameter and configuration files
    └── CRMCPPTest.sln              ← Visual Studio solution (all projects)
```

---

## Algorithm Overview

| Component | Method |
|---|---|
| IVP integration | 4th-order Adams-Bashforth-Moulton predictor-corrector |
| BVP solver | Shooting method |
| Nonlinear solver | MINPACK Trust-Region Dogleg (custom C++ port) |
| Jacobian | Analytical (default) and numerical finite-difference |

The primary use case is real-time control loops; both the C++ static library and the Python bindings are designed with that performance requirement in mind.

---

## Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| **Eigen** | 5.0.1+ | Header-only; see installation below |
| **C++ compiler** | C++17 | MSVC (Windows) or GCC/Clang (Linux) |
| **Python** *(optional)* | 3.8+ | Required only for Python bindings |
| **nanobind** *(optional)* | 2.x | `pip install nanobind`; required only for Python bindings |
| **numpy** *(optional)* | any | `pip install numpy`; required only for Python bindings |

### Installing Eigen

Download Eigen from <https://libeigen.gitlab.io/> and place (or clone) it into a folder named `eigen5` **alongside** this repository:

```
CatheterProject/
├── eigen5/          ← unpack/clone Eigen here
└── CRM_CPPImplementation/
```

The code has been tested with Eigen 5.0.1. No build step is required — Eigen is header-only.

---

## Building on Windows (Visual Studio)

Open `CRMCPPTest.sln` in Visual Studio 2022 or 2026. The solution contains three projects:

| Project | Output | Description |
|---|---|---|
| `CRMCPPLib` | `CRMCPPLib.lib` | Core kinematics static library |
| `CRMCPPTest` | `CRMCPPTest.exe` | Console application exercising the full API |
| `CRMPythonBindings` | `crm.pyd` | Python extension module (requires nanobind) |

Select **Release \| x64** and press **F7** (Build All). `CRMCPPTest.exe` has no external runtime dependencies beyond the CRT.

To build only the Python module, right-click **CRMPythonBindings → Build** after installing nanobind (`pip install nanobind`). The `.pyd` is placed in the `python/` folder automatically. See [`python/README.md`](python/README.md) for details.

## Building on Linux

```bash
# from the CRM_CPPImplementation/ directory
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Optionally build Python bindings
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_PYTHON_BINDINGS=ON
cmake --build build
```

See [`python/README.md`](python/README.md) for the full Linux Python build workflow.

---

## C++ API

All symbols are in the `CRMCatheterModel` namespace. Include `src/CRM.hpp`.

### Key Types

```cpp
// Physical description of the catheter
CRMCatheterModelParams  params = Load_CRMCatheterModelParams("path/to/params.txt");

// Spatial configuration (B0 field, gravity, entry port position & orientation)
CatheterConfiguration   config = Load_CatheterConfiguration("path/to/config.txt");

// Contact mode
ContactModeType::FREE_TIP     // free-tip (zero tip force assumed)
ContactModeType::FIXED_TIP    // constrained tip (tip force solved for)

// Segment types (distal → proximal ordering)
CatheterSegmentType::FLEXIBLE
CatheterSegmentType::RIGID_WITH_ACTUATOR
CatheterSegmentType::RIGID
```

### Free Functions

```cpp
// Forward kinematics — Eigen vector interface
//   in_x : [ currents (3 × NUM_ACT_SET) | insertion_length ] (A, mm)
//   returns: [ p(3) | R(9, row-major) | deltau0(3) | ftip(3, FIXED_TIP only) ]
VectorXd CRM_ForwardKinematics(VectorXd in_x, CRMForwardKinematicsData params,
                                double& out_PE, int& out_localmin);

// Analytical hybrid manipulator Jacobian (requires a valid FK solution)
MatrixXd CRM_FKJacobian_Analytical(VectorXd in_x, const VectorXd& fk_out,
                                    CRMForwardKinematicsData params);

// Numerical (finite difference) Jacobian
MatrixXd CRM_FKJacobian_Numerical(VectorXd in_x, VectorXd& fk_out,
                                   CRMForwardKinematicsData params, int& localmin);
```

`out_localmin != 0` indicates the solver stalled at a local minimum or could not make further progress.

### CRM_Catheter Class (recommended for control loops)

The class manages warm-start initial guesses between calls, which is critical for real-time performance.

```cpp
CRM_Catheter catheter(params, config);
catheter.IntegrationStepSize = 0.2;  // mm; trades accuracy for speed

// --- Free-tip forward kinematics (incremental warm-start) ---
auto [ptip, Rtip, deltau0] =
    catheter.ForwardKinematicsFree(actuation_vec, /*CalculateMarkers=*/true,
                                   out_PE, out_localmin);

// --- Constrained-tip forward kinematics (incremental warm-start) ---
auto [ptip, Rtip, deltau0, ftip] =
    catheter.ForwardKinematicsContact(actuation_vec, tip_point,
                                      /*CalculateMarkers=*/true, out_PE, out_localmin);

// --- Analytical Jacobian (uses stored last FK result) ---
MatrixXd Ja = catheter.FKJacobian_Analytical();

// --- Marker and coil data (valid after CalculateMarkers=true) ---
catheter.MarkerPos[i]    // (no_locmarkers × 3) array, mm
catheter.CoilPos[i]      // (no_act_set × 3) array, mm
catheter.CoilOrient[i]   // (no_act_set × 9) array, row-major 3×3

// --- Reset warm-start state (e.g. after a large configuration change) ---
catheter.ResetInitialGuesses();
```

Compile-time constants (set in `CRM.hpp`):

```cpp
NUM_ACT_SET   // number of actuator coil sets (default: 2)
NUM_STATES    // IVP state-vector dimension (default: 15)
```

---

## MATLAB MEX Wrappers

Pre-built `.mexw64` files are in `matlab/`. To rebuild them from source, open MATLAB and run:

```matlab
% Windows
run('matlab/CreateCppMEXFiles_win.m')

% Ubuntu
run('matlab/CreateCppMEXFiles_ubuntu.m')
```

A full usage example mirroring the C++ test application is in `matlab/TestCppMEXFiles.m`.

---

## Python Bindings

A nanobind-based Python module (`crm.pyd` / `crm.so`) exposes the full C++ API with NumPy array interop. See [`python/README.md`](python/README.md) for build instructions and the full API reference.

Quick start after building:

```python
import crm, numpy as np

params = crm.Load_CRMCatheterModelParams("../catheterdata/CatheterParameterSet_2.txt")
config = crm.Load_CatheterConfiguration("../catheterdata/CatheterSpatialConfiguration_1.txt")

catheter = crm.CRM_Catheter(params, config)
catheter.IntegrationStepSize = 0.2

actuation = np.zeros(3 * crm.NUM_ACT_SET + 1)
actuation[-1] = 80.0  # insertion length, mm

ptip, Rtip, deltau0, PE, localmin = catheter.ForwardKinematicsFree(actuation, True)
print("Tip position:", ptip)
```

---

## Sample Data Files

Located in `catheterdata/`:

| File | Description |
|---|---|
| `CatheterParameterSet_1.txt` | Catheter physical parameters — configuration 1 |
| `CatheterParameterSet_2.txt` | Catheter physical parameters — configuration 2 |
| `CatheterSpatialConfiguration_1.txt` | Spatial configuration (B0, gravity, entry port) |

---

## References

The details of the model can be found in the below references:
1.
2.
3.
4.

