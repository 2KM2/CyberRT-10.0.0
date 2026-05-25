ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ARG USERNAME=developer
ARG USER_UID=1000
ARG USER_GID=1000

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
    cmake\
    software-properties-common\
    gcc-9 \
    g++-9\
    && python3 -m pip install --no-cache-dir protobuf==3.14.0 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --gid "${USER_GID}" "${USERNAME}" \
    && useradd --uid "${USER_UID}" --gid "${USER_GID}" --create-home --shell /bin/bash "${USERNAME}" \
    && usermod -aG sudo "${USERNAME}" \
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" >/etc/sudoers.d/${USERNAME} \
    && chmod 0440 /etc/sudoers.d/${USERNAME}

WORKDIR /workspace/CyberRT-10.0.0
USER ${USERNAME}
CMD ["/bin/bash"]
