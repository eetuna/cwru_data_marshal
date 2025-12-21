# .devcontainer/Dockerfile
#
# Multi-stage image:
# - build-base: full toolchain + ISMRMRD build/install
# - dev: adds debugger/CLI tooling and defaults to non-root "vscode"
# - runtime: lean runtime deps (optional)

ARG UBUNTU_VERSION=22.04

# =========================
# Base build environment
# =========================
FROM mcr.microsoft.com/devcontainers/base:ubuntu-22.04 AS build-base

ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_ISMRMRD_FROM_SOURCE=true
ARG ISMRMRD_TAG=main

RUN apt-get update && \
  apt-get install -y --no-install-recommends \
  ca-certificates \
  git \
  curl \
  jq \
  iproute2 \
  net-tools \
  ninja-build \
  lld \
  build-essential \
  cmake \
  pkg-config \
  libssl-dev \
  libhdf5-dev \
  libboost-filesystem-dev \
  libboost-program-options-dev \
  libboost-system-dev \
  libboost-thread-dev \
  libeigen3-dev \
  # libopencv-dev ships OpenCVConfig.cmake, the split packages pull in viz + GUI bits without
  # dragging every optional OpenCV module via Recommends.
  libopencv-dev \
  libopencv-core-dev \
  libopencv-highgui-dev \
  libopencv-imgcodecs-dev \
  libopencv-imgproc-dev \
  libopencv-viz-dev \
  libpugixml-dev \
  python3 \
  python3-dev \
  python3-pip \
  python3-venv \
  && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --no-cache-dir --upgrade pip && \
  python3 -m pip install --no-cache-dir ismrmrd h5py numpy websockets requests

ENV CMAKE_GENERATOR=Ninja
ENV CMAKE_EXE_LINKER_FLAGS_INIT="-fuse-ld=lld"
ENV CMAKE_SHARED_LINKER_FLAGS_INIT="-fuse-ld=lld"
ENV CMAKE_LINK_PARALLEL_JOBS=1

# ---- ISMRMRD (source or apt) ----
RUN set -eux; \
  if [ "${BUILD_ISMRMRD_FROM_SOURCE}" = "true" ]; then \
    git clone --depth 1 --branch "${ISMRMRD_TAG}" https://github.com/ismrmrd/ismrmrd.git /opt/ismrmrd || \
    git clone --depth 1 https://github.com/ismrmrd/ismrmrd.git /opt/ismrmrd; \
    cmake -S /opt/ismrmrd -B /opt/ismrmrd/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DUSE_SYSTEM_HDF5=ON; \
    cmake --build /opt/ismrmrd/build -j"$(nproc)"; \
    cmake --install /opt/ismrmrd/build; \
  else \
    apt-get update; \
    apt-get install -y --no-install-recommends libismrmrd-dev || true; \
    rm -rf /var/lib/apt/lists/*; \
    if ! pkg-config --exists ismrmrd; then \
      git clone --depth 1 --branch "${ISMRMRD_TAG}" https://github.com/ismrmrd/ismrmrd.git /opt/ismrmrd || \
      git clone --depth 1 https://github.com/ismrmrd/ismrmrd.git /opt/ismrmrd; \
      cmake -S /opt/ismrmrd -B /opt/ismrmrd/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DUSE_SYSTEM_HDF5=ON; \
      cmake --build /opt/ismrmrd/build -j"$(nproc)"; \
      cmake --install /opt/ismrmrd/build; \
    fi; \
  fi; \
  ldconfig

ENV CMAKE_PREFIX_PATH=/usr/local

# =========================
# Dev stage (tools + debugger)
# =========================
FROM build-base AS dev

RUN apt-get update && apt-get install -y --no-install-recommends \
  gdb \
  less \
  vim-tiny \
  clang \
  lldb \
  gnupg \
  wget \
  && rm -rf /var/lib/apt/lists/*

# Install Node.js 22 (NodeSource) + global CLIs
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - && \
  apt-get update && \
  apt-get install -y --no-install-recommends nodejs && \
  rm -rf /var/lib/apt/lists/*

RUN node -v && npm -v && \
  npm install -g @google/gemini-cli @openai/codex @anthropic-ai/claude-code

# Install websocat (static musl build, works on amd64/arm64)
ARG WEBSOCAT_VERSION=v1.13.0
RUN set -eux; \
  arch="$(dpkg --print-architecture)"; \
  case "$arch" in \
    amd64) asset="websocat.x86_64-unknown-linux-musl" ;; \
    arm64) asset="websocat.aarch64-unknown-linux-musl" ;; \
    *) echo "Unsupported arch: $arch" >&2; exit 1 ;; \
  esac; \
  url="https://github.com/vi/websocat/releases/download/${WEBSOCAT_VERSION}/${asset}"; \
  curl -fsSL "$url" -o /usr/local/bin/websocat; \
  chmod +x /usr/local/bin/websocat

RUN mkdir -p /workspaces/cwru_data_marshal && \
  mkdir -p /home/vscode/.cache /home/vscode/.config/claude /home/vscode/.claude && \
  chown -R vscode:vscode /workspaces /home/vscode

ENV SHELL=/bin/bash
WORKDIR /workspaces/cwru_data_marshal

USER vscode
CMD ["/bin/bash"]

# =========================
# Runtime (lean) stage
# =========================
FROM ubuntu:${UBUNTU_VERSION} AS runtime
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
  ca-certificates \
  curl \
  jq \
  iproute2 \
  net-tools \
  libpugixml1v5 \
  libhdf5-103-1 \
  libhdf5-hl-200 \
  libboost-system1.74.0 \
  libboost-filesystem1.74.0 \
  libboost-thread1.74.0 \
  libboost-program-options1.74.0 \
  # Jammy (22.04) ships OpenCV 4.5 runtimes under the "4.5d" suffix.
  libopencv-core4.5d \
  libopencv-highgui4.5d \
  libopencv-imgcodecs4.5d \
  libopencv-imgproc4.5d \
  libopencv-viz4.5d \
  python3 \
  python3-pip \
  && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --no-cache-dir --upgrade pip && \
  python3 -m pip install --no-cache-dir ismrmrd h5py numpy websockets requests

# Optional: install websocat in runtime too
ARG WEBSOCAT_VERSION=v1.13.0
RUN set -eux; \
  arch="$(dpkg --print-architecture)"; \
  case "$arch" in \
    amd64) asset="websocat.x86_64-unknown-linux-musl" ;; \
    arm64) asset="websocat.aarch64-unknown-linux-musl" ;; \
    *) echo "Unsupported arch: $arch" >&2; exit 1 ;; \
  esac; \
  url="https://github.com/vi/websocat/releases/download/${WEBSOCAT_VERSION}/${asset}"; \
  curl -fsSL "$url" -o /usr/local/bin/websocat; \
  chmod +x /usr/local/bin/websocat

COPY --from=build-base /usr/local /usr/local

ENV LD_LIBRARY_PATH=/usr/local/lib
ENV CMAKE_PREFIX_PATH=/usr/local
ENV CMAKE_GENERATOR=Ninja
ENV CMAKE_EXE_LINKER_FLAGS_INIT="-fuse-ld=lld"
ENV CMAKE_SHARED_LINKER_FLAGS_INIT="-fuse-ld=lld"
ENV CMAKE_LINK_PARALLEL_JOBS=1

WORKDIR /app

