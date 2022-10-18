FROM ubuntu:18.04

RUN apt update & apt install -y build-essential cmake clang openssl libssl-dev zlib1g-dev gperf wget git curl libreadline-dev ccache libmicrohttpd-dev ninja-build

WORKDIR /

RUN git clone --recurse-submodules https://github.com/ton-blockchain/ton.git

WORKDIR /ton
RUN mkdir /ton/build
WORKDIR /ton/build
ENV CC clang
ENV CXX clang++
RUN cmake -GNinja -DCMAKE_BUILD_TYPE=Release ..
RUN ninja tonlibjson blockchain-explorer fift func validator-engine validator-engine-console create-state generate-random-id create-hardfork dht-server create-state lite-client