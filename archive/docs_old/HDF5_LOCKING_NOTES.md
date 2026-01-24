# HDF5 File Locking Notes

## Summary

The MRI demo uses HDF5 to write `.mrd` files. HDF5 tries to take OS-level file locks by default. On WSL/overlayfs (including Docker Desktop on Windows), those locks can fail with `errno=11` and crash the demo. To keep the demo stable, the simultaneous demo scripts disable HDF5 file locking **only on WSL**.

## Current Behavior (Demo Scripts)

In:
- `scripts/run_demo_simultaneous.sh`
- `scripts/run_demo_simultaneous_noninteractive.sh`

The scripts check `/proc/version` and if it contains `microsoft` or `wsl`, they set:

```
HDF5_USE_FILE_LOCKING=FALSE
HDF5_FILE_LOCKING=FALSE
```

On native Linux, no overrides are applied and file locking remains enabled.

## Docker Behavior

- **Docker on native Linux**: `/proc/version` does not contain `microsoft`, so locking stays **enabled**.
- **Docker Desktop on Windows (WSL2 backend)**: `/proc/version` contains `microsoft`, so locking is **disabled** to avoid crashes.

## Manual Overrides

You can override the demo behavior by setting these in your shell **before** running the demo:

Enable locking:
```
export HDF5_USE_FILE_LOCKING=TRUE
export HDF5_FILE_LOCKING=TRUE
```

Disable locking:
```
export HDF5_USE_FILE_LOCKING=FALSE
export HDF5_FILE_LOCKING=FALSE
```

## SWMR Impact

Disabling OS-level locks does **not** disable SWMR semantics inside HDF5, but it removes the extra protection that prevents two writers from opening the same file. It is safe as long as there is only one writer process.
