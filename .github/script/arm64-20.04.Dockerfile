FROM ubuntu:20.04

RUN apt update
RUN DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get -y install tzdata
RUN apt install -y build-essential cmake clang openssl libssl-dev zlib1g-dev gperf wget git curl libreadline-dev ccache libmicrohttpd-dev ninja-build

WORKDIR /

ARG BRANCH
RUN git clone --recurse-submodules https://github.com/ton-blockchain/ton.git && cd ton && git checkout $BRANCH

WORKDIR /ton
RUN mkdir /ton/build
WORKDIR /ton/build
ENV CC clang
ENV CXX clang++
ENV CCACHE_DISABLE 1
RUN cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DPORTABLE=1 -DTON_ARCH= ..
RUN ninja storage-daemon storage-daemon-cli tonlibjson blockchain-explorer fift func validator-engine validator-engine-console create-state generate-random-id dht-server lite-client