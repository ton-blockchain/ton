FROM alpine:20190925 AS build

WORKDIR /ton
COPY ./ .

WORKDIR /ton-build
RUN apk add --no-cache cmake g++ gflags-dev linux-headers make openssl-dev zlib-dev
RUN cmake /ton
RUN cmake --build . --target func

WORKDIR /artifacts/lib
RUN cp /lib/ld-musl-x86_64.so.1 /lib/libcrypto.so.1.1 /lib/libz.so.1 .
WORKDIR /artifacts/usr/lib
RUN cp /usr/lib/libstdc++.so.6 /usr/lib/libgcc_s.so.1 .
WORKDIR /artifacts/usr/local/bin
RUN cp /ton-build/crypto/func .
WORKDIR /artifacts/usr/local/lib/func
RUN cp -r /ton/crypto/smartcont/stdlib.fc .

FROM scratch
COPY --from=build /artifacts /
WORKDIR /app
ENTRYPOINT ["func"]
