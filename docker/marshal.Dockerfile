# docker/marshal.Dockerfile
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
libstdc++6 libboost-system1.74.0 curl \
&& rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/marshal /app/marshal
WORKDIR /app
CMD ["/app/marshal", "--http", "0.0.0.0:8080", "--ws", "0.0.0.0:8090", "--data", "/data"]