FROM alpine:20190925 AS build

WORKDIR /ton
COPY ./ .
RUN ln -s third-party/rocksdb/include .

WORKDIR /ton-build
RUN apk add --no-cache cmake g++ gflags-dev linux-headers make openssl-dev zlib-dev
RUN cmake /ton
RUN cmake --build . --target fift

WORKDIR /artifacts/lib
RUN cp /lib/ld-musl-x86_64.so.1 /lib/libcrypto.so.1.1 /lib/libz.so.1 .
WORKDIR /artifacts/usr/lib
RUN cp /usr/lib/libstdc++.so.6 /usr/lib/libgcc_s.so.1 .
WORKDIR /artifacts/usr/local/bin
RUN cp /ton-build/crypto/fift .
WORKDIR /artifacts/usr/local/lib
RUN cp -r /ton/crypto/fift/lib fift


FROM scratch
COPY --from=build /artifacts /
ENV FIFTPATH=/usr/local/lib/fift
WORKDIR /app
ENTRYPOINT ["fift"]
