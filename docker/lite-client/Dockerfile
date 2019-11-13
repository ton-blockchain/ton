FROM alpine:20190925 AS build

WORKDIR /ton
COPY ./ .

WORKDIR /ton-build
RUN apk add --no-cache cmake g++ gflags-dev linux-headers make openssl-dev zlib-dev
RUN cmake /ton
RUN cmake --build . --target lite-client



FROM alpine:20190925

RUN apk add --no-cache libgcc libstdc++ openssl zlib
COPY --from=build /ton-build/lite-client/lite-client /usr/local/bin/lite-client

WORKDIR /app
ENTRYPOINT ["lite-client"]
