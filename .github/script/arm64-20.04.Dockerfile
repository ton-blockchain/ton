FROM ubuntu:20.04

RUN apt update
RUN DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get -y install tzdata
RUN apt install -y build-essential git make cmake clang libgflags-dev libreadline-dev pkg-config libgsl-dev python3 python3-dev ninja-build automake autogen libtool texinfo

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
RUN cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DPORTABLE=1 -DTON_ARCH= ..
RUN ninja storage-daemon storage-daemon-cli tonlibjson blockchain-explorer fift func validator-engine validator-engine-console create-state generate-random-id dht-server lite-client