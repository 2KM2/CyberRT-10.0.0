# Ubuntu 24.04 Development Image

This is the minimal CyberRT compile/development environment based on Ubuntu 24.04.

Build:

```bash
scripts/docker_ubuntu24_dev.sh build
```

Run with the repository mounted:

```bash
scripts/docker_ubuntu24_dev.sh run
```

Installed packages:

```bash
apt-get install -y wget g++ pkg-config autoconf automake \
    libcurl4-openssl-dev uuid-dev libncurses5-dev \
    libtool python3-dev python3-pip libtiff-dev \
    libeigen3-dev libsqlite3-dev sqlite3
python3 -m pip install protobuf==3.14.0
```
