ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive
ENV PIP_BREAK_SYSTEM_PACKAGES=1

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
    sudo \
    wget \
    git \
    g++ \
    make \ 
    patch \
    gdb\
    gdbserver\
    pkg-config \
    autoconf \
    automake \
    libcurl4-openssl-dev \
    uuid-dev \
    libncurses5-dev \
    libtool \
    python3-dev \
    python3-pip \
    libtiff-dev \
    libeigen3-dev \
    libsqlite3-dev \
    sqlite3 \
    && python3 -m pip install --no-cache-dir protobuf==3.14.0 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace/CyberRT-10.0.0
CMD ["/bin/bash"]
