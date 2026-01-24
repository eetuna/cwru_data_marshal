# Quick Start - Docker Demo

## 1. Run the Demo

```bash
# Grant GUI access (for visualization window)
xhost +local:docker

# Run 30-second demo
./scripts/demo-docker.sh
```

That's it! The demo will:
- Start 10 containers (2 marshals, 8 clients)
- Stream MRI images, ECG data, and pose tracking
- Show visualization window
- Run for 30 seconds and clean up

---

## 2. See Generated Data in Your Workspace

By default, data is in a Docker volume (not visible in workspace).

**Option A: Copy data out after demo**

```bash
# Set CLEANUP_DATA=false first (keeps data after demo)
sed -i 's/CLEANUP_DATA=true/CLEANUP_DATA=false/' .env.demo

# Run demo
./scripts/demo-docker.sh

# Copy data to workspace
docker cp cwru-mri-marshal:/data/mri_data/ ./session-data/

# Now you can see it
ls -lh session-data/mrd/
```

**Option B: Mount workspace directory directly**

Edit `docker-compose.demo.yml` line 12, change:
```yaml
    volumes:
      - mri-data:/data/mri_data
```
To:
```yaml
    volumes:
      - ./session-data:/data/mri_data
```

Then:
```bash
# Create directory
mkdir -p session-data

# Run demo
./scripts/demo-docker.sh

# Data appears in workspace automatically
ls -lh session-data/mrd/
```

---

## 3. Common Commands

```bash
# Build images first time
docker compose -f docker-compose.demo.yml build

# Start services manually
docker compose -f docker-compose.demo.yml up -d

# Watch logs
docker compose -f docker-compose.demo.yml logs -f

# Stop everything
docker compose -f docker-compose.demo.yml down

# Test API
curl http://localhost:8080/health
```

---

## 4. Customize Settings

Edit `.env.demo`:

```bash
IMAGE_WIDTH=128          # Change resolution
IMAGE_HEIGHT=128
DEMO_DURATION=60         # Run for 60 seconds
ENABLE_VIZ=false         # Disable GUI window
CLEANUP_DATA=false       # Keep data after demo
```

---

## 5. Troubleshooting

**No visualization window?**
```bash
xhost +local:docker
docker compose -f docker-compose.demo.yml restart viz-client
```

**Port already in use?**
```bash
docker compose -f docker-compose.demo.yml down
```

**Can't connect to Docker?**
```bash
sudo systemctl start docker
```

---

See [DOCKER_DEMO_STEPS.md](DOCKER_DEMO_STEPS.md) for detailed documentation.
