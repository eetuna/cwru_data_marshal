
# docker/Dockerfile
ARG UBUNTU_VERSION=22.04
FROM ubuntu:${UBUNTU_VERSION} AS build-base

ARG DEBIAN_FRONTEND=noninteractive
# ARG BUILD_ISMRMRD_FROM_SOURCE=false
# ARG ISMRMRD_TAG=v1.14.0
ARG BUILD_ISMRMRD_FROM_SOURCE=true
ARG ISMRMRD_TAG=main

RUN apt-get update && \
  apt-get install -y --no-install-recommends \
  ca-certificates \
  git \
  curl \
  jq \
  ninja-build \
  lld \
  libpugixml1v5 \
  # build deps typical for this project (you already have most elsewhere; keep your original list)

  build-essential \
  cmake \
  pkg-config \
  libssl-dev \
  libhdf5-dev \
  libboost-all-dev \
  libpugixml-dev \

  && apt-get clean && apt-get clean && rm -rf /var/lib/apt/lists/*

# Make Ninja the default generator inside the image
ENV CMAKE_GENERATOR=Ninja
# Favor lld linker (fast  lower memory spikes)
ENV CMAKE_EXE_LINKER_FLAGS_INIT="-fuse-ld=lld"
ENV CMAKE_SHARED_LINKER_FLAGS_INIT="-fuse-ld=lld"
# Reduce peak memory during large link steps
ENV CMAKE_LINK_PARALLEL_JOBS=1

RUN set -eux; \
  if [ "${BUILD_ISMRMRD_FROM_SOURCE}" = "true" ]; then \
  echo "Building ISMRMRD from source (forced)"; \
  git clone --depth 1 --branch "${ISMRMRD_TAG}" https://github.com/ismrmrd/ismrmrd.git /opt/ismrmrd || \
  git clone --depth 1 https://github.com/ismrmrd/ismrmrd.git /opt/ismrmrd; \
  cmake -S /opt/ismrmrd -B /opt/ismrmrd/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DUSE_SYSTEM_HDF5=ON; \
  cmake --build /opt/ismrmrd/build -j"$(nproc)"; \
  cmake --install /opt/ismrmrd/build; \
  else \
  echo "Trying to install ISMRMRD via apt, falling back to source if missing"; \
  apt-get update; \
  # Try dev package if repo has it; do NOT try a non-existent 'ismrmrd' package.
  apt-get install -y --no-install-recommends libismrmrd-dev || true; \
  apt-get clean; rm -rf /var/lib/apt/lists/*; \
  if ! pkg-config --exists ismrmrd; then \
  echo "libismrmrd-dev not available or insufficient; building from source"; \
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

FROM build-base AS dev
RUN apt-get update && apt-get install -y --no-install-recommends \
  gdb \
  vim \
  && apt-get clean && apt-get clean && rm -rf /var/lib/apt/lists/*
WORKDIR /src

FROM ubuntu:${UBUNTU_VERSION} AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
  ca-certificates \
  curl \
  jq \
  ninja-build \
  lld \
  libpugixml1v5 \

  libhdf5-103-1 \
  libboost-system1.83.0 libboost-filesystem1.83.0 libboost-thread1.83.0 libboost-program-options1.83.0 \
  && apt-get clean && apt-get clean && rm -rf /var/lib/apt/lists/*
COPY --from=build-base /usr/local /usr/local
ENV LD_LIBRARY_PATH=/usr/local/lib
ENV CMAKE_PREFIX_PATH=/usr/local
# Keep the same defaults in runtime too (useful if you build in runtime stage)
ENV CMAKE_GENERATOR=Ninja
ENV CMAKE_EXE_LINKER_FLAGS_INIT="-fuse-ld=lld"
ENV CMAKE_SHARED_LINKER_FLAGS_INIT="-fuse-ld=lld"
ENV CMAKE_LINK_PARALLEL_JOBS=1
WORKDIR /app
