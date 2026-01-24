# WSL2 Quick Start

## Run Demo in WSL2

```bash
# 1. Run the demo
./scripts/demo-docker.sh

# 2. Data appears in ./session-data/
ls -lh session-data/mrd/
```

That's it!

---

## Optional: Enable Visualization (GUI)

If you want to see the visualization window:

### Install X Server on Windows

Download and install one of:
- **VcXsrv**: https://sourceforge.net/projects/vcxsrv/
- **Xming**: https://sourceforge.net/projects/xming/
- **Or use WSLg** (Windows 11 only - built-in)

### Configure X Server

1. Start X server on Windows
2. In WSL2, allow Docker to access X:

```bash
export DISPLAY=:0
xhost +local:docker
```

3. Run demo:

```bash
./scripts/demo-docker.sh
```

---

## Common Commands

```bash
# Build images
docker compose -f docker-compose.demo.yml build

# Start without GUI
sed -i 's/ENABLE_VIZ=true/ENABLE_VIZ=false/' .env.demo
./scripts/demo-docker.sh

# Keep data between runs
sed -i 's/CLEANUP_DATA=true/CLEANUP_DATA=false/' .env.demo
./scripts/demo-docker.sh

# View logs
docker compose -f docker-compose.demo.yml logs -f

# Stop all
docker compose -f docker-compose.demo.yml down
```

---

## Where's the Data?

**In WSL2:** `./session-data/mrd/`

Generated files:
- `demo_stream-64x64x5-g0000.mrd` - MRI images
- `bio.jsonl` - ECG data
- `poses.jsonl` - Pose tracking
- `index.jsonl` - Frame metadata
- `latest.json` - Latest frame

Access from Windows: `\\wsl$\Ubuntu\workspaces\cwru_data_marshal\session-data\`

---

## Test APIs

```bash
# MRI Marshal
curl -s http://localhost:8080/health

# Robot Marshal
curl -s http://localhost:8081/
```

---

See [docs/QUICK_START.md](docs/QUICK_START.md) for more details.
