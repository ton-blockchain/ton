FROM ubuntu:20.04 as builder
RUN apt-get update && \
	DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake clang-6.0 openssl libssl-dev zlib1g-dev gperf wget git ninja-build && \
	rm -rf /var/lib/apt/lists/*
ENV CC clang-6.0
ENV CXX clang++-6.0
ENV CCACHE_DISABLE 1
WORKDIR /
RUN git clone --recursive https://github.com/ton-blockchain/ton
WORKDIR /ton

RUN mkdir build && \
	cd build && \
	cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DPORTABLE=1 -DTON_ARCH= -DCMAKE_CXX_FLAGS="-mavx2" .. && \
	ninja storage-daemon storage-daemon-cli tonlibjson fift func validator-engine validator-engine-console generate-random-id dht-server lite-client

FROM ubuntu:20.04
RUN apt-get update && \
	apt-get install -y openssl wget libatomic1 && \
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
COPY init.sh control.template ./
RUN chmod +x init.sh

ENTRYPOINT ["/var/ton-work/db/init.sh"]