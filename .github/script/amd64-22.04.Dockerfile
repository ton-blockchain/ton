FROM ubuntu:22.04

RUN apt update
RUN DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get -y install tzdata
RUN apt install -y build-essential cmake clang openssl libssl-dev zlib1g-dev gperf wget git curl libreadline-dev ccache libmicrohttpd-dev ninja-build libsecp256k1-dev libsodium-dev pkg-config

WORKDIR /

ARG BRANCH
ARG REPO
RUN git clone --recurse-submodules https://github.com/$REPO ton && cd ton && git checkout $BRANCH && git submodule update

WORKDIR /ton
RUN mkdir /ton/build
WORKDIR /ton/build
ENV CC clang
ENV CXX clang++
ENV CCACHE_DISABLE 1
RUN cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DPORTABLE=1 -DTON_ARCH= -DCMAKE_CXX_FLAGS="-mavx2" ..
RUN ninja storage-daemon storage-daemon-cli tonlibjson blockchain-explorer fift func tolk validator-engine validator-engine-console create-state generate-random-id create-hardfork dht-server lite-client
