cmake_minimum_required(VERSION 3.16)

add_executable(ConsensusHarness
    ConsensusHarness.cpp
    GraphLogger.cpp
)

target_compile_features(ConsensusHarness PRIVATE cxx_std_17)

target_include_directories(ConsensusHarness PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# No external dependencies — pure C++17 stdlib only.
# Add threading support for std::mutex
find_package(Threads REQUIRED)
target_link_libraries(ConsensusHarness PRIVATE Threads::Threads)

set_target_properties(ConsensusHarness PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/simulation"
)
