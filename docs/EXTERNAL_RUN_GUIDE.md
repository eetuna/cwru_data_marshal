# External User Run Guide (Umbrella)

This branch is an umbrella runner. It does **not** contain MRI or robot marshal source code. It pulls them from dedicated branches/repositories and runs them together.

## Quick Start (Local)

1) Clone this repo and enter it.
2) Run the noninteractive demo:

```
./scripts/run_demo_simultaneous_noninteractive.sh
```

The script will:
- create a git worktree for MRI marshal from `mri-data-marhsal`
- create a git worktree for robot marshal (catheter) from `robot_data_marshal_with_catheter_system_components`
- build both and run them together

## Worktree Locations

Defaults:
- MRI marshal worktree: `../mri_data_marshal_worktree`
- Robot marshal worktree: `../robot_data_marshal_catheter_worktree`

Override with:
```
export MRI_MARSHAL_DIR=/path/to/mri-marshal-repo
export ROBOT_MARSHAL_DIR=/path/to/robot-marshal-repo
```

## Demo Scripts

- Interactive demo:
  ```
  ./scripts/run_demo.sh
  ```
- Simultaneous demo (interactive):
  ```
  ./scripts/run_demo_simultaneous.sh
  ```
- Simultaneous demo (noninteractive, 30s):
  ```
  ./scripts/run_demo_simultaneous_noninteractive.sh
  ```

## Environment Overrides

MRI marshal:
- `MRI_MARSHAL_DIR` (repo/worktree path)
- `MRI_MARSHAL_BIN`, `MRI_IMAGE_STREAMER_BIN`, `MRI_VIZ_CLIENT_BIN`, `MRI_MK_MRD_BIN`, `MRI_PLAYBACK_BIN`

Robot marshal (catheter):
- `ROBOT_MARSHAL_DIR` (repo/worktree path)
- `ROBOT_MARSHAL_BIN` (server binary)
- `ROBOT_CLIENTS` (optional: newline-delimited `name:/path/to/bin` list)

Example:
```
export MRI_MARSHAL_DIR=/opt/mri
export ROBOT_MARSHAL_DIR=/opt/robot
./scripts/run_demo_simultaneous_noninteractive.sh
```

## Docker Compose

Build and run both marshals:
```
docker compose up --build
```

This uses:
- MRI marshal from `mri-data-marhsal`
- Robot marshal (catheter) from `robot_data_marshal_with_catheter_system_components`

## Export Docker Images (USB Transfer)

```
./scripts/export_images.sh
```

Artifacts:
- `exported_images/mri-marshal.tar`
- `exported_images/robot-marshal.tar`

On another machine:
```
docker load -i mri-marshal.tar
docker load -i robot-marshal.tar
```

## What Runs Where

- MRI marshal (HTTP 8080, WS 8090)
- Robot marshal (HTTP 8081)
- Clients and demo tools run locally from the umbrella scripts

## Troubleshooting

- Missing binaries: ensure the worktrees exist or set `MRI_MARSHAL_DIR` / `ROBOT_MARSHAL_DIR`.
- WSL HDF5 lock errors: the demo auto-disables locking on WSL; native Linux keeps locking enabled.
- GUI not showing new frames: each run uses a unique data directory under `data_demo_mri/`.
