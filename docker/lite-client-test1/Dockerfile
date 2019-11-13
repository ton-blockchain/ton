FROM alpine:20190925 AS build

WORKDIR /ton
COPY ./ .

WORKDIR /ton-build
RUN apk add --no-cache cmake g++ gflags-dev linux-headers make openssl-dev zlib-dev
RUN cmake /ton
RUN cmake --build . --target lite-client
RUN wget https://test.ton.org/ton-lite-client-test1.config.json



FROM alpine:20190925

RUN apk add --no-cache libgcc libstdc++ openssl zlib
COPY --from=build /ton-build/lite-client/lite-client            /usr/local/bin/lite-client
COPY --from=build /ton-build/ton-lite-client-test1.config.json  /var/local/lib/lite-client/ton-lite-client-test1.config.json

WORKDIR /app
ENTRYPOINT ["lite-client", "-C", "/var/local/lib/lite-client/ton-lite-client-test1.config.json"]
