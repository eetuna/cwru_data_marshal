# CRM MATLAB MEX Wrappers

MATLAB MEX interface for the CRM Catheter Kinematics Library.
Pre-built `.mexw64` binaries for Windows are included. Source files can be
recompiled on Windows or Ubuntu using the provided build scripts.

---

## Prerequisites

| Dependency | Notes |
|---|---|
| **MATLAB** | R2018a or later (MEX `-R2018a` API used) |
| **MEX C++ compiler** | MSVC on Windows; GCC on Ubuntu. Run `mex -setup C++` to configure. |
| **Eigen5** | Required only to recompile from source. See below. |

### Eigen for recompilation

**Windows** — Eigen is expected at the sibling location used by the VS projects:
```
CatheterProject/
├── eigen5/          ← Eigen headers
└── CRM_CPPImplementation/
    └── matlab/
```

**Ubuntu** — The build script expects Eigen at `/usr/local/include/eigen3/`.
Install with:
```bash
sudo apt install libeigen3-dev
```
Or adjust the `-I` path in `CreateCppMEXFiles_ubuntu.m` if Eigen is elsewhere.

---

## Building the MEX Files from Source

> Pre-built `.mexw64` files are already present. Recompilation is only needed
> after modifying `src/` or `numerical/` C++ sources, or when building on Ubuntu.

Open MATLAB, `cd` into the `matlab/` folder, then run the appropriate script:

**Windows:**
```matlab
run('CreateCppMEXFiles_win.m')
```

**Ubuntu:**
```matlab
run('CreateCppMEXFiles_ubuntu.m')
```

Both scripts compile all five MEX functions with `NUM_ACT_SET=2` and C++17.
The resulting binaries are written to the `matlab/` folder.

---

## The FKParams Structure

Most functions accept an `FKParams` structure. Assemble it as follows:

```matlab
% --- Catheter description and configuration (load once) ---
FKParams.CathParams  = Load_CRMCatheterModelParams_matlab('../catheterdata/CatheterParameterSet_2.txt');
FKParams.CathConfig  = Load_CatheterConfiguration_matlab('../catheterdata/CatheterSpatialConfiguration_1.txt');

% --- Contact mode ---
FKParams.ContactMode = int8(0);   % 0 = FREE_TIP,  1 = FIXED_TIP

% --- Tip force (FREE_TIP only) [unit: N, spatial coordinates] ---
FKParams.TipForce = [0.0, 0.0, 0.0];

% --- Tip constraint point (FIXED_TIP only) [unit: mm, spatial coordinates] ---
FKParams.TipConstraintPoint = [0.0, 0.0, 0.0];

% --- Output control ---
FKParams.FinalValueOnly = false;  % false: also return marker/coil positions

% --- Numerical integration step size [unit: mm] ---
FKParams.IntegrationStepSize = 0.2;

% --- Initial guesses for the nonlinear solver ---
FKParams.deltau0_initialguess = [0.0, 0.0, 0.0];  % delta curvature at catheter base [rad/mm]
FKParams.ftip_initialguess    = [0.0, 0.0, 0.0];  % tip force [N], used when FIXED_TIP
```

---

## MEX Function Reference

### `Load_CRMCatheterModelParams_matlab`

```matlab
CathParams = Load_CRMCatheterModelParams_matlab(path)
```

Loads catheter physical parameters from a text file into a MATLAB struct.

| Argument | Description |
|---|---|
| `path` | Path to the parameter file (e.g. `'../catheterdata/CatheterParameterSet_2.txt'`) |
| **Returns** | `CathParams` struct — stored in `FKParams.CathParams` |

---

### `Load_CatheterConfiguration_matlab`

```matlab
CathConfig = Load_CatheterConfiguration_matlab(path)
```

Loads the catheter's spatial configuration (B0 field, gravity, entry port) from file.

| Argument | Description |
|---|---|
| `path` | Path to the configuration file (e.g. `'../catheterdata/CatheterSpatialConfiguration_1.txt'`) |
| **Returns** | `CathConfig` struct — stored in `FKParams.CathConfig` |

---

### `CRM_ForwardKinematics_matlab`

```matlab
[FKsolution, PotentialEnergy, ReportedMarkerPos, ReportedCoilPos, ReportedCoilOrient, localmin] = ...
    CRM_ForwardKinematics_matlab(control_inputs, FKParams)
```

Solves the CRM forward kinematics boundary value problem.

| Argument | Description |
|---|---|
| `control_inputs` | Row vector `[I1x I1y I1z ... INx INy INz  InsertedLength]` — actuation currents (A) for each coil axis of each actuator set, followed by the inserted catheter length (mm). Length = `3 × NUM_ACT_SET + 1`. |
| `FKParams` | Parameter structure (see above) |

| Output | Description |
|---|---|
| `FKsolution` | Column vector. **FREE_TIP:** `[p(3); R(9); deltau0(3)]`. **FIXED_TIP:** `[p(3); R(9); deltau0(3); ftip(3)]`. `R` is a 3×3 rotation matrix in row-major order. |
| `PotentialEnergy` | Scalar (mJ) — total potential energy (strain + magnetic + gravitational) |
| `ReportedMarkerPos` | `(no_locmarkers × 3)` array of localization marker positions (mm). Empty if `FKParams.FinalValueOnly = true`. |
| `ReportedCoilPos` | `(no_act_set × 3)` array of actuator coil centre positions (mm) |
| `ReportedCoilOrient` | `(no_act_set × 9)` array of actuator coil orientation matrices (row-major) |
| `localmin` | `0` = converged; `≠ 0` = solver stalled at local minimum or could not make further progress |

---

### `CRM_FKJacobian_Analytical_matlab`

```matlab
Ja = CRM_FKJacobian_Analytical_matlab(control_inputs, FKsolution, FKParams)
```

Computes the analytical hybrid manipulator Jacobian at the current configuration.
Must be called with the same `control_inputs` and `FKParams` that produced `FKsolution`.

| Argument | Description |
|---|---|
| `control_inputs` | Same vector passed to `CRM_ForwardKinematics_matlab` |
| `FKsolution` | Output of `CRM_ForwardKinematics_matlab` for an equilibrium configuration |
| `FKParams` | Same parameter structure used for the FK call |

| Output | Shape | Contents |
|---|---|---|
| `Ja` (FREE_TIP) | `(6) × (3×NUM_ACT_SET+1+3)` | `[dp/dz, dp/dft; ws_dz, ws_dft]`. First 6 rows = hybrid manipulator Jacobian. |
| `Ja` (FIXED_TIP) | `(3) × (3×NUM_ACT_SET+1)` | `dftip/dz` — partial derivative of tip force w.r.t. inputs |

---

### `CRM_CalculateCatheterEnergy_matlab`

```matlab
PotentialEnergy = CRM_CalculateCatheterEnergy_matlab(deltau0, ftip, control_inputs, FKParams)
```

Evaluates the catheter's total potential energy for a given state without solving the BVP.
Useful for energy landscape analysis and verifying solver convergence.

| Argument | Description |
|---|---|
| `deltau0` | `[3×1]` delta curvature at the catheter base (rad/mm) |
| `ftip` | `[3×1]` tip force in spatial coordinates (N). Use `[0 0 0]` for FREE_TIP. |
| `control_inputs` | Same format as `CRM_ForwardKinematics_matlab` |
| `FKParams` | Parameter structure |
| **Returns** | `PotentialEnergy` — scalar (mJ) |

---

## Example Scripts

### `TestCppMEXFiles.m`

Comprehensive test script covering all major use cases:

- **Free-tip deflection** — loads parameters, sets currents and insertion length,
  calls `CRM_ForwardKinematics_matlab`, prints `FKsolution`, marker positions,
  coil positions/orientations, and the Jacobian, then renders the catheter shape.
- **Energy calculation (free tip)** — compares `PE` at the initial guess vs. the
  converged solution.
- **Constrained-tip deflection** — repeats the above with `ContactMode = 1` and
  a specified `TipConstraintPoint`.
- **Energy calculation (constrained tip)** — same energy comparison for the
  contact scenario.

Run from the `matlab/` folder:
```matlab
run('TestCppMEXFiles.m')
```

### `CatheterMotionTest.m`

Animates catheter deflection over a sweep of actuation currents and insertion
lengths. Uses `DrawCRMCatheterModel` for real-time rendering.

### `DrawCRMCatheterModel.m`

Utility function used by the example scripts to render the catheter shape,
marker positions, coil orientations, and the B0 field vector.

```matlab
DrawCRMCatheterModel(FKParams, control_inputs, draw_mag_vector)
% Or with pre-computed results (faster — skips re-running FK):
DrawCRMCatheterModel(FKParams, control_inputs, draw_mag_vector, ...
    ReportedMarkerPos, ReportedCoilPos, ReportedCoilOrient, segment_steps)
```

### `SampleEnergyAnalysis.mlx`

MATLAB Live Script demonstrating energy landscape analysis using
`CRM_CalculateCatheterEnergy_matlab`.

---

## Notes

- **Segment ordering** — all arrays in `CathParams` (segment lengths, types,
  radii, etc.) are ordered **distal to proximal** (tip → base).
- **`NUM_ACT_SET`** — compiled into the MEX binaries as a preprocessor constant
  (default `2`). If your catheter has a different number of actuator sets,
  recompile with `-DNUM_ACT_SET=N` in the build scripts.
- **Units** — lengths in mm, currents in A, forces in N, moments in N·mm,
  potential energy in mJ, curvatures in rad/mm.
