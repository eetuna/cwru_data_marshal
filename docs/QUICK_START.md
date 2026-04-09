# Quick Start

One-time alias (paste into your shell):

```bash
alias cdd='docker compose --env-file .env.demo -f docker-compose.demo.yml'
```

## Flow A — real reconstruction (k-space → recon → image)

```bash
cdd up -d mri-marshal
cdd up -d robot-marshal
cdd up -d mock-recon
cdd --profile viz up -d viz-client
cdd up -d ecg-client
cdd up -d pose-client
cdd --profile robot-clients up -d robot-clients
cdd up -d kspace-streamer
```

## Flow B — bypass reconstruction (pre-made images)

```bash
cdd up -d mri-marshal
cdd up -d robot-marshal
cdd --profile viz up -d viz-client
cdd up -d ecg-client
cdd up -d pose-client
cdd --profile robot-clients up -d robot-clients
cdd up -d image-streamer
```

## Stop everything

```bash
cdd --profile viz --profile robot-clients down
```

## Clear the sticky latest frame (between sessions)

```bash
curl -X DELETE http://localhost:8080/v1/mrd/latest
```

## Service reference (for both flows unless noted)

| Service | Profile | Role |
|---|---|---|
| `mri-marshal` | — | MRI data hub (HTTP 8080, WS 8090) |
| `robot-marshal` | — | Robot data hub (HTTP 8081) |
| `mock-recon` | — | Reconstruction service (real python-ismrmrd-server). **Flow A only.** |
| `image-streamer` | — | Pre-made image producer. **Flow B only.** |
| `kspace-streamer` | — | Raw k-space producer. **Flow A only.** |
| `ecg-client` | — | Synthetic ECG samples to marshal. Both flows. |
| `pose-client` | — | Synthetic pose/tracking data to marshal. Both flows. |
| `viz-client` | `viz` | OpenCV image viewer. Polls `/v1/mrd/latest`. |
| `robot-clients` | `robot-clients` | Bundle of catheter-tracking, controller, planning, front-end, surface-tracking. |

Individual robot clients (if you want to run them separately instead of as the `robot-clients` bundle): `catheter-tracking`, `controller`, `planning`, `front-end`, `surface-tracking`. All no-profile, each takes the same `cdd up -d <name>` form.

## Rules

- Always use `cdd` (or pass `--env-file .env.demo`). Without it, marshal starts without `--recon-endpoint` and every k-space POST returns 501.
- Flow A and Flow B are mutually exclusive. Do not run `image-streamer` together with `mock-recon`/`kspace-streamer`.
- `mock-recon` must be up before `kspace-streamer`, otherwise marshal returns 501 until it appears.
- `viz-client` and `robot-clients` live behind compose profiles (`viz`, `robot-clients`). They only start when the matching `--profile` flag is on the command.
