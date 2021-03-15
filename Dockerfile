FROM ubuntu:18.04 as builder
RUN apt-get update && \
    apt-get install -y cmake clang ninja-build pkg-config libssl-dev zlib1g-dev libreadline-dev libmicrohttpd-dev gperf git wget && \
    rm -rf /var/lib/apt/lists/*
COPY . /ton/
WORKDIR /ton/build
RUN cmake -G "Ninja" ..
RUN ninja
RUN wget https://test.ton.org/ton-lite-client-test1.config.json
RUN wget https://test.ton.org/ton-global.config.json

FROM ubuntu:18.04
RUN apt-get update && \
    apt-get install -y openssl libreadline7 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=builder /ton/build/lite-client/lite-client \
                    /ton/build/crypto/fift \
                    /ton/build/crypto/tlbc \
                    /ton/build/crypto/create-state \
                    /ton/build/crypto/dump-block \
                    /ton/build/crypto/func \
                    /ton/build/validator-engine/validator-engine \
                    /ton/build/validator-engine-console/validator-engine-console \
                    /ton/build/tonlib/tonlib-cli \
                    /ton/build/utils/generate-random-id \
                    /ton/build/adnl/adnl-proxy \
                    /ton/build/adnl/adnl-pong \
                    /ton/build/tdnet/tcp_ping_pong \
                    /ton/build/tdnet/udp_ping_pong \
                    /ton/build/dht-server/dht-server \
                    /ton/build/ton-lite-client-test1.config.json \
                    /ton/build/ton-global.config.json \
                    /usr/bin/
COPY --from=builder /ton/crypto/fift/lib/* \
                    /ton/crypto/smartcont/stdlib.fc \
                    /ton/build/commit \
                    /usr/include/ton/
ENV FIFTPATH=/usr/include/ton
ENV FUNCPATH=/usr/include/ton
WORKDIR /data