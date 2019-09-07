#!/bin/sh
./src.sh | xargs -n 1 clang-format -verbose -style=file -i
