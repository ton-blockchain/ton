FROM alpine:20190925 AS build

WORKDIR /ton
COPY ./ .

WORKDIR /ton-build
RUN apk add --no-cache cmake g++ gflags-dev linux-headers make openssl-dev zlib-dev
RUN cmake /ton
RUN cmake --build . --target fift



FROM alpine:20190925

RUN apk add --no-cache libgcc libstdc++ openssl zlib
COPY --from=build /ton-build/crypto/fift  /usr/local/bin/fift
COPY --from=build /ton/crypto/fift/lib    /usr/local/lib/fift

ENV FIFTPATH=/usr/local/lib/fift
WORKDIR /app
ENTRYPOINT ["fift"]
