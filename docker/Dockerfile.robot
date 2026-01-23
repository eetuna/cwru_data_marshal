FROM ubuntu:22.04

ARG REPO_URL=https://github.com/cwru-mercis/cwru_data_marshal.git
ARG BRANCH=robot_data_marshal_with_catheter_system_components

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    git \
    python3 \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "$BRANCH" "$REPO_URL" /opt/robot

# Patch server.cpp to listen on 0.0.0.0:8081 instead of hardcoded 172.28.1.10:8080
RUN sed -i 's/server.listen("172.28.1.10", 8080)/server.listen("0.0.0.0", 8081)/' /opt/robot/server.cpp

# Build the server
RUN g++ -std=c++17 -I /opt/robot /opt/robot/server.cpp \
    -o /opt/robot/server -lpthread

# Create log directory required by server
RUN mkdir -p /opt/robot/log_files

WORKDIR /opt/robot
VOLUME ["/data/robot_data"]
EXPOSE 8081

HEALTHCHECK --interval=10s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -f http://localhost:8081/read/robot_status || exit 1

CMD ["./server"]
