FROM ubuntu:20.04

ENV TZ=Asia/Shanghai
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

ENV DEBIAN_FRONTEND=noninteractive

# Use Aliyun mirror for faster downloads in China
RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.aliyun.com|g' /etc/apt/sources.list && \
    sed -i 's|http://security.ubuntu.com|http://mirrors.aliyun.com|g' /etc/apt/sources.list

# Exactly match unitree_sdk2 devcontainer + README requirements
RUN apt-get update && apt-get install -y \
    build-essential gcc-9 g++-9 cmake \
    libyaml-cpp-dev libeigen3-dev libboost-all-dev libspdlog-dev libfmt-dev \
    patchelf \
    && rm -rf /var/lib/apt/lists/*

# Force gcc-9 / g++-9 as default (critical for Cyclone DDS headers)
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-9 100

WORKDIR /build
