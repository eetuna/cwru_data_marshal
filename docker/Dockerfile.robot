FROM ubuntu:22.04

ARG REPO_URL=https://github.com/cwru-mercis/cwru_data_marshal.git
ARG BRANCH=robot_data_marshal_with_catheter_system_components

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    git \
    python3 \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "$BRANCH" "$REPO_URL" /opt/robot

RUN mkdir -p /opt/robot/build \
    && g++ -std=c++17 -I /opt/robot /opt/robot/server.cpp \
        -o /opt/robot/build/robot_marshal_demo -lpthread

COPY docker/robot_entrypoint.sh /opt/robot/robot_entrypoint.sh
RUN chmod +x /opt/robot/robot_entrypoint.sh

WORKDIR /opt/robot
VOLUME ["/files"]
EXPOSE 8081

CMD ["/opt/robot/robot_entrypoint.sh"]
