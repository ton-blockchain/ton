FROM ubuntu:22.04

RUN apt update
RUN DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get -y install tzdata
RUN apt install -y build-essential cmake clang openssl libssl-dev zlib1g-dev gperf wget git curl libreadline-dev ccache libmicrohttpd-dev ninja-build

WORKDIR /

RUN git clone --recurse-submodules https://github.com/ton-blockchain/ton.git

WORKDIR /ton
RUN mkdir /ton/build
WORKDIR /ton/build
ENV CC clang
ENV CXX clang++
ENV CCACHE_DISABLE 1
RUN cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DTON_ARCH= ..
RUN ninja tonlibjson blockchain-explorer fift func validator-engine validator-engine-console create-state generate-random-id create-hardfork dht-server lite-client