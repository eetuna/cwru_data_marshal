# Docker Architecture: Container Startup Methods

## Overview

When starting a Docker container, there are two main approaches:
1. **Direct CMD** - Run the executable directly (our choice)
2. **Entrypoint Script** - Run a shell script that configures and starts the executable

---

## 1. Direct CMD (Our Current Approach)

### What It Is
The container runs the server binary directly without any wrapper script.

### File Structure
```
docker/
├── Dockerfile.mri
└── Dockerfile.robot
```

### Example (Dockerfile.robot)
```dockerfile
# Build the server
RUN g++ -std=c++17 server.cpp -o server -lpthread

# Run it directly
CMD ["./server"]
```

### Pros
- Simple and easy to understand
- Fewer files to maintain
- Fewer things that can break

### Cons
- Configuration is fixed at build time
- To change settings, must rebuild the image

---

## 2. Entrypoint Script (Alternative Approach)

### What It Is
The container runs a shell script that:
1. Reads environment variables
2. Configures the application
3. Starts the server

### File Structure
```
docker/
├── Dockerfile.mri
├── Dockerfile.robot
└── robot_entrypoint.sh    # Extra script file
```

### Example (robot_entrypoint.sh)
```bash
#!/bin/bash
PORT=${ROBOT_PORT:-8081}
echo "Starting on port $PORT"
exec ./server $PORT
```

### Example (Dockerfile.robot with entrypoint)
```dockerfile
COPY robot_entrypoint.sh /opt/robot/
RUN chmod +x /opt/robot/robot_entrypoint.sh
ENTRYPOINT ["/opt/robot/robot_entrypoint.sh"]
```

### Pros
- Runtime configuration via environment variables
- Can wait for dependencies before starting
- Custom signal handling

### Cons
- More complex
- Extra file to maintain
- Script errors can be hard to debug

---

## Why We Chose Direct CMD

| Consideration | Our Situation |
|---------------|---------------|
| Port numbers | Fixed (8080, 8081, 8090) |
| Dependencies | None - marshals start independently |
| Configuration | Same for all deployments |
| Audience | Handover - simpler is better |

---

## Cleanup Note

The file `docker/robot_entrypoint.sh` exists but is **not used**. It can be deleted:

```bash
rm docker/robot_entrypoint.sh
```
