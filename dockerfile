FROM ubuntu:20.04

ENV DEBIAN_FRONTEND noninteractive

WORKDIR /root
RUN apt-get update \
    && apt-get -y --quiet --no-install-recommends install \
        autoconf \
        automake \
        autotools-dev \
        build-essential \
        cmake \
        git \
        libcurl4-openssl-dev \
        libltdl-dev \
        libtinyxml2-dev \
        libtool \
        libz-dev \
        ninja-build \
        python3 \
        python3-pip \
        python3-future \
        software-properties-common \
        wget \
    && apt-get -y autoremove \
    && apt-get clean autoclean \
    && rm -rf /var/lib/apt/lists/{apt,dpkg,cache,log} /tmp/* /var/tmp/*

# MAVSDK
RUN wget https://github.com/mavlink/MAVSDK/releases/download/v1.4.17/libmavsdk-dev_1.4.17_ubuntu20.04_amd64.deb
RUN dpkg -i libmavsdk-dev_1.4.17_ubuntu20.04_amd64.deb

WORKDIR /root/MAVSDK
COPY . .

WORKDIR /root/MAVSDK/examples/multiple_drones_monitor/
RUN cmake -Bbuild -H.
RUN cmake --build build -j4

ENTRYPOINT ["/bin/bash"]
