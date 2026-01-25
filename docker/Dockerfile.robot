FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    curl \
    ca-certificates \
    python3 \
    && rm -rf /var/lib/apt/lists/*

# Copy source files from build context (robot worktree)
COPY server.cpp httplib.h json.hpp circularBuffer.hpp /opt/robot/
COPY files.json /opt/robot/
COPY file*.json /opt/robot/

# Create directories - /files is where the server looks for data files
RUN mkdir -p /opt/robot/log_files /files

# Build the server (supports --http command-line arg, no sed patch needed)
RUN g++ -std=c++17 /opt/robot/server.cpp -o /opt/robot/server -lpthread

WORKDIR /opt/robot

# Initialize all data files with seed data (required for robot clients to work)
# The server reads from /files/ directory - each file needs valid JSON
RUN python3 -c "import json, os; \
    seed = {'client_id': 'seed', 'sent_at': 1, 'values': [1.0, 2.0, 3.0]}; \
    files = json.load(open('files.json')); \
    [open(f'/files/{f}', 'w').write(json.dumps(seed)) for f in files]"

# Data volume mount point
VOLUME ["/data/robot_data"]

# Expose HTTP port
EXPOSE 8081

# Health check - just verify server responds
HEALTHCHECK --interval=10s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -sf http://localhost:8081/ || exit 1

# Default command (can override with --http in docker-compose)
CMD ["./server", "--http", "0.0.0.0:8081"]
