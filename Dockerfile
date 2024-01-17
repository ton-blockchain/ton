FROM ubuntu:22.04 as builder
RUN apt-get update && \
	DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake clang openssl libssl-dev zlib1g-dev gperf wget git ninja-build libsecp256k1-dev libsodium-dev libmicrohttpd-dev pkg-config autoconf automake libtool && \
	rm -rf /var/lib/apt/lists/*
ENV CC clang
ENV CXX clang++
ENV CCACHE_DISABLE 1

WORKDIR /
RUN mkdir ton
WORKDIR /ton

COPY ./ ./

RUN mkdir build && \
	cd build && \
	cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DPORTABLE=1 -DTON_ARCH= .. && \
	ninja storage-daemon storage-daemon-cli tonlibjson fift func validator-engine validator-engine-console generate-random-id dht-server lite-client

FROM ubuntu:22.04
RUN apt-get update && \
    apt-get install -y wget libatomic1 openssl libsecp256k1-dev libsodium-dev libmicrohttpd-dev && \
    rm -rf /var/lib/apt/lists/*

RUN mkdir -p /var/ton-work/db && \
    mkdir -p /var/ton-work/db/static

COPY --from=builder /ton/build/storage/storage-daemon/storage-daemon /usr/local/bin/
COPY --from=builder /ton/build/storage/storage-daemon/storage-daemon-cli /usr/local/bin/
COPY --from=builder /ton/build/lite-client/lite-client /usr/local/bin/
COPY --from=builder /ton/build/validator-engine/validator-engine /usr/local/bin/
COPY --from=builder /ton/build/validator-engine-console/validator-engine-console /usr/local/bin/
COPY --from=builder /ton/build/utils/generate-random-id /usr/local/bin/

WORKDIR /var/ton-work/db
COPY ./docker/init.sh ./docker/control.template ./
RUN chmod +x init.sh

ENTRYPOINT ["/var/ton-work/db/init.sh"]