# .devcontainer/Dockerfile
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive

# Base tools + deps (ninja optional but nice)
RUN apt-get update && apt-get install -y \
    build-essential cmake git curl pkg-config ninja-build \
    libboost-all-dev \
    libhdf5-dev libxml2-dev zlib1g-dev \
    libpugixml-dev \
    gdb valgrind ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# ---------- ISMRMRD from source ----------
WORKDIR /opt
# You can pin to a tag, e.g. v1.14.0 — leaving master if you need latest:
# RUN git clone --depth 1 --branch v1.14.0 https://github.com/ismrmrd/ismrmrd.git
RUN git clone https://github.com/ismrmrd/ismrmrd.git

RUN cmake -S /opt/ismrmrd -B /opt/ismrmrd/build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DBUILD_TESTS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DUSE_SYSTEM_HDF5=ON \
    && cmake --build /opt/ismrmrd/build -j \
    && cmake --install /opt/ismrmrd/build

# Make sure the runtime can see newly installed libs
RUN ldconfig

# Help CMake find /usr/local packages like ISMRMRDConfig.cmake
ENV CMAKE_PREFIX_PATH=/usr/local

WORKDIR /src
