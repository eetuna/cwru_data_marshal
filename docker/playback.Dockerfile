# docker/playback.Dockerfile
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
libstdc++6 libboost-system1.74.0 libhdf5-103-1 libismrmrd2 \
&& rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/playback /app/playback
WORKDIR /app
CMD ["/app/playback", "--http", "http://marshal:8080", "--ws", "ws://marshal:8090/ws", "--data", "/data"]