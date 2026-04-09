# Quick Start

One-time:

```bash
alias cdd='docker compose --env-file .env.demo -f docker-compose.demo.yml'
```

## Flow A — real reconstruction (k-space → recon → image)

```bash
cdd up -d mri-marshal
cdd up -d robot-marshal
cdd up -d mock-recon
cdd --profile viz up -d viz-client
cdd up -d robot-clients
cdd up -d kspace-streamer
```

## Flow B — bypass reconstruction (pre-made images)

```bash
cdd up -d mri-marshal
cdd up -d robot-marshal
cdd --profile viz up -d viz-client
cdd up -d robot-clients
cdd up -d image-streamer
```

## Stop everything

```bash
cdd down
```

## Clear the sticky latest frame (between sessions)

```bash
curl -X DELETE http://localhost:8080/v1/mrd/latest
```

## Rules

- Always use `cdd` (or pass `--env-file .env.demo`). Without it, marshal starts without `--recon-endpoint` and every k-space POST returns 501.
- Flow A and Flow B are mutually exclusive. Do not run `image-streamer` together with `mock-recon` / `kspace-streamer`.
- `mock-recon` must be up before `kspace-streamer`, otherwise marshal returns 501 until it appears.
