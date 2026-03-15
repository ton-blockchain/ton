#!/bin/bash
export PATH=/ucrt64/bin:/usr/bin:/bin
cd /c/GitHub/tonGraph/build
ninja third-party/zlib/lib/libz.a -j4
echo "ZLIB_EXIT:$?"
ninja ConsensusHarness -j4
echo "NINJA_EXIT:$?"
