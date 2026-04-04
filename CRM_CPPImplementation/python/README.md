# CRM Python Bindings

Python bindings for the CRM Catheter Kinematics Library, built with [nanobind](https://github.com/wjakob/nanobind).

---

## Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| **Python** | 3.8+ | 64-bit recommended |
| **numpy** | any | `pip install numpy` |
| **nanobind** | 2.x | `pip install nanobind` |
| **Eigen5** | 5.0.1+ | Already at `../eigen5`; no install needed |
| **C++ compiler** | C++17 | MSVC on Windows, GCC/Clang on Linux |

---

## Building on Windows (Visual Studio)

### Option A — Visual Studio Solution (recommended)

The `CRMPythonBindings` project is already part of `CRMCPPTest.sln`. Python and
nanobind paths are detected automatically by `CRMPythonBindings.props` — no
manual path configuration is needed.

1. Install nanobind:
   ```
   pip install nanobind
   ```
2. Open `CRMCPPTest.sln` in Visual Studio 2022 or 2026.
3. Set the active configuration to **Release \| x64**.
4. Right-click **CRMPythonBindings** in Solution Explorer → **Build**.
5. `crm.pyd` is placed in the `python/` folder next to `test_crm.py`.

> **Path auto-detection** (`CRMPythonBindings.props`) searches for Python and
> nanobind in this order:
> 1. Environment variables `PYTHONDIR` / `NANOBINDDIR`
> 2. Windows registry (HKLM then HKCU)
> 3. `%APPDATA%\Python\Python3xx\site-packages\nanobind` (per-user `pip install`)
> 4. `<PythonDir>\Lib\site-packages\nanobind` (system-wide `pip install`)
>
> If auto-detection fails, set `PYTHONDIR` and/or `NANOBINDDIR` as user
> environment variables before opening Visual Studio.

### Option B — CMake via Open Folder

1. Install nanobind:
   ```
   pip install nanobind
   ```
2. In Visual Studio: **File → Open → Folder...** and select the `python/` folder.
3. Visual Studio detects `CMakePresets.json` and shows the available build
   presets in the toolbar. Select **Windows x64 Release (VS 2026)** (or
   **VS 2022** if that is your version).
4. **Build → Build All** (or **F7**).
5. **Build → Install crm** to copy `crm.pyd` next to `test_crm.py`.

### Option C — Developer PowerShell

1. In Visual Studio: **Tools → Command Line → Developer PowerShell**.
2. Run:
   ```powershell
   cd <path_to_repo>\python
   cmake -B build --preset windows-vs2026
   cmake --build build --config Release
   cmake --install build --config Release
   ```
   Replace `windows-vs2026` with `windows-vs2022` if using VS 2022.

---

## Building on Linux (Ubuntu)

1. Install prerequisites:
   ```bash
   pip install nanobind numpy
   ```
2. Configure, build, and install:
   ```bash
   cd python
   cmake --preset linux-release
   cmake --build --preset linux-release
   cmake --install build --config Release
   ```

---

## Running the Example

After building, `crm.pyd` (Windows) or `crm.so` (Linux) will be in the
`python/` folder.

```bash
cd python
python test_crm.py
```

---

## API Reference

All functions and classes are in the `crm` module.

### Loading Parameters

```python
# Load catheter physical parameters from file
params = crm.Load_CRMCatheterModelParams(path: str) -> crm.CRMCatheterModelParams

# Load catheter spatial configuration from file
config = crm.Load_CatheterConfiguration(path: str) -> crm.CatheterConfiguration
```

### Free Functions

```python
# Forward kinematics — Eigen vector interface
# in_x : numpy array of length (3 * NUM_ACT_SET + 1)
#         [ currents (A, row-major) | insertion_length (mm) ]
# Returns (out_y, PE, localmin)
#   out_y     : numpy array [ p(3) | R(9,row-major) | deltau0(3) ] for FREE_TIP
#               numpy array [ p(3) | R(9,row-major) | deltau0(3) | ftip(3) ] for FIXED_TIP
#   PE        : float, potential energy (mJ)
#   localmin  : int, 0 = converged; != 0 = stalled at local minimum
out_y, PE, localmin = crm.CRM_ForwardKinematics(in_x, params, config,
                          contact_mode, tip_constraint, tip_force,
                          deltau0_guess, ftip_guess, step_size)

# Analytical hybrid manipulator Jacobian
# Requires a valid FK solution (out_y from CRM_ForwardKinematics)
# Returns Jacobian matrix as numpy array
Ja = crm.CRM_FKJacobian_Analytical(in_x, out_y, params, config,
         contact_mode, tip_constraint, tip_force,
         deltau0_guess, ftip_guess, step_size)

# Numerical (finite difference) Jacobian
# Returns (Jn, out_y, localmin)
Jn, out_y, localmin = crm.CRM_FKJacobian_Numerical(in_x, params, config,
                           contact_mode, tip_constraint, tip_force,
                           deltau0_guess, ftip_guess, step_size)
```

`localmin != 0` indicates the solver stalled at a local minimum or could not
make further progress.

### CRM_Catheter Class

The class manages warm-start initial guesses between calls, which is important
for real-time and iterative use.

```python
catheter = crm.CRM_Catheter(params, config)
catheter.IntegrationStepSize = 0.2  # mm; default 1.0; smaller = more accurate

# --- Free-tip forward kinematics (incremental warm-start) ---
# Returns (ptip, Rtip, deltau0, PE, localmin)
ptip, Rtip, deltau0, PE, localmin = catheter.ForwardKinematicsFree(
    actuation,              # numpy array, length (3 * NUM_ACT_SET + 1)
    CalculateMarkers=True)  # update MarkerPos / CoilPos / CoilOrient

# With explicit initial guess and non-zero tip force:
ptip, Rtip, deltau0, PE, localmin = catheter.ForwardKinematicsFree(
    actuation, tip_force, deltau0_guess, CalculateMarkers=True)

# --- Constrained-tip forward kinematics (incremental warm-start) ---
# Returns (ptip, Rtip, deltau0, ftip, PE, localmin)
ptip, Rtip, deltau0, ftip, PE, localmin = catheter.ForwardKinematicsContact(
    actuation, tip_constraint_point, CalculateMarkers=True)

# With explicit initial guesses:
ptip, Rtip, deltau0, ftip, PE, localmin = catheter.ForwardKinematicsContact(
    actuation, tip_constraint_point,
    deltau0_guess, ftip_guess, CalculateMarkers=True)

# --- Analytical Jacobian (uses result of last FK call) ---
Ja = catheter.FKJacobian_Analytical()

# --- Marker and coil positions (available after CalculateMarkers=True) ---
markers     = catheter.MarkerPos    # (no_locmarkers x 3) numpy array, mm
coil_pos    = catheter.CoilPos      # (no_act_set x 3) numpy array, mm
coil_orient = catheter.CoilOrient   # (no_act_set x 9) numpy array, row-major 3x3

# --- Reset warm-start state ---
catheter.ResetInitialGuesses()
```

#### Jacobian Output Layout

| Last FK call | Output shape | Contents |
|---|---|---|
| `ForwardKinematicsFree` | `(6, 3*NUM_ACT_SET+1+3)` | `[dp/dz, dp/dft; ws_dz, ws_dft]` — first 6 rows = hybrid manipulator Jacobian |
| `ForwardKinematicsContact` | `(3, 3*NUM_ACT_SET+1)` | `dftip/dz` |

### Enums

```python
crm.ContactModeType.FREE_TIP          # no tip contact
crm.ContactModeType.FIXED_TIP         # tip constrained to a point

crm.CatheterSegmentType.FLEXIBLE
crm.CatheterSegmentType.RIGID_WITH_ACTUATOR
crm.CatheterSegmentType.RIGID
```

### Constants

```python
crm.NUM_ACT_SET   # number of actuator coil sets (compile-time constant, default 2)
crm.NUM_STATES    # IVP state-vector dimension (compile-time constant, default 15)
```

---

## Quick-Start Example

```python
import crm
import numpy as np

params = crm.Load_CRMCatheterModelParams("../catheterdata/CatheterParameterSet_2.txt")
config = crm.Load_CatheterConfiguration("../catheterdata/CatheterSpatialConfiguration_1.txt")

catheter = crm.CRM_Catheter(params, config)
catheter.IntegrationStepSize = 0.2

# Control input: 6 currents (A) + insertion length (mm)
actuation = np.array([0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 100.0])

ptip, Rtip, deltau0, PE, localmin = catheter.ForwardKinematicsFree(actuation, True)
print("Tip position (mm):", ptip)
print("Potential energy (mJ):", PE)
print("Converged:", localmin == 0)

Ja = catheter.FKJacobian_Analytical()
print("Jacobian shape:", Ja.shape)
```

See `test_crm.py` for a complete example covering both free-tip and
constrained-tip modes.
