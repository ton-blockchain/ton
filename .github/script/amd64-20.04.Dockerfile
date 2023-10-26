FROM ubuntu:20.04

RUN apt update
RUN DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get -y install tzdata
RUN apt install -y build-essential cmake clang openssl libssl-dev zlib1g-dev gperf wget git curl libreadline-dev ccache libmicrohttpd-dev ninja-build pkg-config

WORKDIR /

ARG BRANCH
ARG REPO
RUN git clone --recurse-submodules https://github.com/$REPO && cd ton && git checkout $BRANCH

WORKDIR /ton
RUN mkdir /ton/build
WORKDIR /ton/build
ENV CC clang
ENV CXX clang++
ENV CCACHE_DISABLE 1
RUN cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DPORTABLE=1 -DTON_ARCH= -DCMAKE_CXX_FLAGS="-mavx2" ..
RUN ninja storage-daemon storage-daemon-cli tonlibjson blockchain-explorer fift func validator-engine validator-engine-console create-state generate-random-id create-hardfork dht-server lite-client