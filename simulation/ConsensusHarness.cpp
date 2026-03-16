// ConsensusHarness.cpp — simulation harness stub
// Full Byzantine scenarios will be implemented here.
// This file intentionally minimal so the build target exists.

#include "GraphLogger.h"
#include <cstdio>
#include <cstring>

using namespace ton::simulation;

int main(int argc, char* argv[]) {
    const char* scenario = "equivocation";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario = argv[++i];
        }
    }

    GraphLogger::instance().init("harness-session", "simulation/trace.ndjson");

    GraphEntry e;
    e.node_id  = make_node_id();
    e.type     = "HarnessStart";
    e.ts_ms    = 0;
    e.slot     = 0;
    e.outcome  = scenario;

    GraphLogger::instance().emit(e);
    std::printf("ConsensusHarness: scenario=%s, trace written to simulation/trace.ndjson\n", scenario);
    return 0;
}
