FROM sickcodes/docker-osx:Monterey

WORKDIR /

RUN git clone --recurse-submodules https://github.com/ton-blockchain/ton.git

WORKDIR /ton
RUN mkdir /ton/build
WORKDIR /ton/build
ENV CC="clang -mcpu=apple-a14"
ENV CXX="clang++ -mcpu=apple-a14"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j 4 fift
