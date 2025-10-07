# docker/Dockerfile
ARG UBUNTU_VERSION=22.04

# =========================
# Base build environment
# =========================
FROM ubuntu:${UBUNTU_VERSION} AS build-base

ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_ISMRMRD_FROM_SOURCE=true
ARG ISMRMRD_TAG=main

RUN apt-get update && \
  apt-get install -y --no-install-recommends \
  ca-certificates \
  git \
  curl \
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
  libopencv-core-dev \
  libopencv-highgui-dev \
  libopencv-imgcodecs-dev \
  libopencv-imgproc-dev \
  libpugixml-dev \
  python3 \
  python3-pip \
  && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --no-cache-dir --upgrade pip && \
    python3 -m pip install --no-cache-dir ismrmrd h5py numpy

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
  && rm -rf /var/lib/apt/lists/*

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

WORKDIR /src

# =========================
# Runtime (lean) stage
# =========================
FROM ubuntu:${UBUNTU_VERSION} AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
  ca-certificates \
  curl \
  libpugixml1v5 \
  libhdf5-103-1 \
  libhdf5-hl-200 \
  libboost-system1.74.0 \
  libboost-filesystem1.74.0 \
  libboost-thread1.74.0 \
  libboost-program-options1.74.0 \
  libopencv-core406t64 \
  libopencv-highgui406t64 \
  libopencv-imgcodecs406t64 \
  libopencv-imgproc406t64 \
  python3 \
  python3-pip \
  && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --no-cache-dir --upgrade pip && \
    python3 -m pip install --no-cache-dir ismrmrd h5py numpy

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
