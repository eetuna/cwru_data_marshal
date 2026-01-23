FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Copy source files from build context (robot worktree)
COPY server.cpp httplib.h json.hpp circularBuffer.hpp /opt/robot/
COPY files.json /opt/robot/
COPY file*.json /opt/robot/

# Create directories
RUN mkdir -p /opt/robot/log_files /files

# Patch server.cpp to listen on 0.0.0.0:8081 instead of hardcoded 172.28.1.10:8080
RUN sed -i 's/server.listen("172.28.1.10", 8080)/server.listen("0.0.0.0", 8081)/' /opt/robot/server.cpp

# Build the server
RUN g++ -std=c++17 /opt/robot/server.cpp -o /opt/robot/server -lpthread

WORKDIR /opt/robot

# Data volume mount point
VOLUME ["/data/robot_data"]

# Expose HTTP port
EXPOSE 8081

# Health check - just verify server responds
HEALTHCHECK --interval=10s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -sf http://localhost:8081/ || exit 1

CMD ["./server"]
