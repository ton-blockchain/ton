FROM alpine:20190925 AS build

WORKDIR /ton
COPY ./ .

WORKDIR /ton-build
RUN apk add --no-cache cmake g++ gflags-dev linux-headers make openssl-dev zlib-dev
RUN cmake /ton
RUN cmake --build . --target lite-client

WORKDIR /artifacts/lib
RUN cp /lib/ld-musl-x86_64.so.1 /lib/libcrypto.so.1.1 /lib/libz.so.1 .
WORKDIR /artifacts/usr/lib
RUN cp /usr/lib/libstdc++.so.6 /usr/lib/libgcc_s.so.1 .
WORKDIR /artifacts/usr/local/bin
RUN cp /ton-build/lite-client/lite-client .
WORKDIR /artifacts/var/local/lib/lite-client
RUN wget https://test.ton.org/ton-lite-client-test1.config.json



FROM scratch
COPY --from=build /artifacts /
WORKDIR /app
ENTRYPOINT ["lite-client", "-C", "/var/local/lib/lite-client/ton-lite-client-test1.config.json"]
