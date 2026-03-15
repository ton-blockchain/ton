// ConsensusHarness.cpp — simulation harness stub
// Full Byzantine scenarios will be implemented here.
// This file intentionally minimal so the build target exists.

#include "GraphLogger.h"
#include <cstdio>

int main() {
    simulation::GraphLogger::init("harness-session", "trace.ndjson");

    simulation::GraphEntry e;
    e.node_id   = simulation::make_node_id();
    e.type      = "HarnessStart";
    e.ts_ms     = 0;
    e.slot      = 0;

    simulation::GraphLogger::emit(e);
    std::printf("ConsensusHarness: trace written to trace.ndjson\n");
    return 0;
}
