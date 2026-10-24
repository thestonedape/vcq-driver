# Toolchain for building vcq.ko and booting it under QEMU on a non-Linux host.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++ make kmod cpio busybox-static \
        qemu-system-x86 linux-image-virtual linux-headers-virtual \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
