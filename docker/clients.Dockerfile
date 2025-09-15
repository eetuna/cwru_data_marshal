FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
libstdc++6 libboost-system1.74.0 \
&& rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/fk_client /app/fk_client
COPY --from=builder /build/viz_client /app/viz_client
WORKDIR /app