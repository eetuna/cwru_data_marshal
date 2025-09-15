
# docker/Dockerfile
ARG UBUNTU_VERSION=22.04
FROM ubuntu:${UBUNTU_VERSION} AS build-base

ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_ISMRMRD_FROM_SOURCE=false
ARG ISMRMRD_TAG=v1.14.0

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      ca-certificates \
      git \
      curl \
      jq \
      libpugixml1v5 \

      build-essential \
      cmake \
      pkg-config \
      libssl-dev \
      libhdf5-dev \
      libboost-all-dev \
      libpugixml-dev \

      && apt-get clean && apt-get clean && rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    if [ "$BUILD_ISMRMRD_FROM_SOURCE" = "true" ]; then \
      git clone --depth 1 --branch "${ISMRMRD_TAG}" https://github.com/ismrmrd/ismrmrd.git /opt/ismrmrd; \
      cmake -S /opt/ismrmrd -B /opt/ismrmrd/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DUSE_SYSTEM_HDF5=ON; \
      cmake --build /opt/ismrmrd/build -j; \
      cmake --install /opt/ismrmrd/build; \
    else \
      apt-get update && apt-get install -y --no-install-recommends libismrmrd-dev ismrmrd || true; \
      apt-get clean && apt-get clean && rm -rf /var/lib/apt/lists/*; \
      if ! pkg-config --exists ismrmrd; then \
        git clone --depth 1 --branch "${ISMRMRD_TAG}" https://github.com/ismrmrd/ismrmrd.git /opt/ismrmrd; \
        cmake -S /opt/ismrmrd -B /opt/ismrmrd/build \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DUSE_SYSTEM_HDF5=ON; \
        cmake --build /opt/ismrmrd/build -j; \
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
      libpugixml1v5 \

      libhdf5-103-1 \
      libboost-system1.83.0 libboost-filesystem1.83.0 libboost-thread1.83.0 libboost-program-options1.83.0 \
      && apt-get clean && apt-get clean && rm -rf /var/lib/apt/lists/*
COPY --from=build-base /usr/local /usr/local
ENV LD_LIBRARY_PATH=/usr/local/lib
ENV CMAKE_PREFIX_PATH=/usr/local
WORKDIR /app
