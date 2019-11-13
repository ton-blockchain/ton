FROM alpine:20190925 AS build

WORKDIR /ton
COPY ./ .

WORKDIR /ton-build
RUN apk add --no-cache cmake g++ gflags-dev linux-headers make openssl-dev zlib-dev
RUN cmake /ton
RUN cmake --build . --target func



FROM alpine:20190925

RUN apk add --no-cache libgcc libstdc++ openssl zlib
COPY --from=build /ton-build/crypto/func           /usr/local/bin/func
COPY --from=build /ton/crypto/smartcont/stdlib.fc  /usr/local/lib/func/stdlib.fc

WORKDIR /app
ENTRYPOINT ["func"]
