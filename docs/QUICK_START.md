# Quick Start

One-time alias (add to `~/.bashrc` so every new shell has it):

```bash
echo "alias cdd='docker compose --env-file .env.demo -f docker-compose.demo.yml'" >> ~/.bashrc && source ~/.bashrc
```

All commands below must be run from the repo root (`cd` into it first) because `cdd` resolves `.env.demo` and `docker-compose.demo.yml` relative to the current directory.

## Flow A -- real reconstruction (k-space to recon to image)

One terminal per service. Each blocks that terminal and streams its logs until `Ctrl-C`.

```bash
# Terminal 1
cdd up mri-marshal

# Terminal 2
cdd up robot-marshal

# Terminal 3
cdd up mock-recon

# Terminal 4
cdd --profile viz up viz-client

# Terminal 5
cdd up pose-client

# Terminal 6
cdd --profile robot-clients up robot-clients

# Terminal 7
cdd up kspace-streamer
```

kspace-streamer sends acquisitions + ECG waveforms (via `--ecg` flag) over MRD TCP.

## Flow B -- bypass reconstruction (pre-made images)

```bash
# Terminal 1
cdd up mri-marshal

# Terminal 2
cdd up robot-marshal

# Terminal 3
cdd --profile viz up viz-client

# Terminal 4
cdd up pose-client

# Terminal 5
cdd --profile robot-clients up robot-clients

# Terminal 6
cdd up image-streamer
```

## Stop everything

```bash
cdd --profile viz --profile robot-clients down
```

## Service reference

| Service | Profile | Role |
|---|---|---|
| `mri-marshal` | -- | MRI data hub (`--http host:port`). |
| `robot-marshal` | -- | Robot data hub (HTTP 8081). |
| `mock-recon` | -- | Reconstruction service. **Flow A only.** |
| `image-streamer` | -- | Pre-made image producer. **Flow B only.** |
| `kspace-streamer` | -- | Raw k-space + ECG waveforms via MRD TCP (`--ecg`). **Flow A only.** |
| `pose-client` | -- | Synthetic pose/tracking data via HTTP POST /pose. Both flows. |
| `viz-client` | `viz` | OpenCV image viewer. Polls `GET /image/latest`. |
| `robot-clients` | `robot-clients` | Bundle of catheter-tracking, controller, planning, front-end, surface-tracking. |

Individual robot clients (run separately instead of as the `robot-clients` bundle): `catheter-tracking`, `controller`, `planning`, `front-end`, `surface-tracking`. All no-profile, each takes the same `cdd up <name>` form.

## Rules

- Always use `cdd` (or pass `--env-file .env.demo`). Without it, marshal starts without `--recon-host`/`--recon-port` and acquisition frames are stored but not forwarded.
- Flow A and Flow B are mutually exclusive. Do not run `image-streamer` together with `mock-recon`/`kspace-streamer`.
- `mock-recon` must be up before `kspace-streamer`, otherwise marshal has nowhere to forward acquisitions.
- `viz-client` and `robot-clients` live behind compose profiles (`viz`, `robot-clients`). They only start when the matching `--profile` flag is on the command.
- Commands shown block their terminal and stream logs. To run detached in the background, add `-d`: `cdd up -d <service>`, then follow logs with `cdd logs -f <service>`. Stop everything (works for both `-d` and blocking runs) with:
  ```bash
  cdd --profile viz --profile robot-clients down
  ```
