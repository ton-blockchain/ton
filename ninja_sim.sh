#!/bin/bash
export PATH=/ucrt64/bin:/usr/bin:/bin
cd /c/GitHub/tonGraph/build
ninja simulation_graph_logger -j4
echo "NINJA_EXIT:$?"
