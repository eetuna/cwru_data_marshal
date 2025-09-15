# Multi-stage builder on Ubuntu 22.04 (Jammy)
FROM ubuntu:22.04 AS builder


ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
build-essential cmake git curl pkg-config \
libboost-all-dev libhdf5-dev libismrmrd-dev \
&& rm -rf /var/lib/apt/lists/*


WORKDIR /src
COPY . /src
RUN cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release \
&& cmake --build /build -j