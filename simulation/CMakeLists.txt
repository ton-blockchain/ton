cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

# simulation/CMakeLists.txt
# Builds GraphLogger as a static library so consensus code can link it.
# The ConsensusHarness executable is only built when WITH_SIMULATION=ON.

add_library(simulation_graph_logger STATIC
  GraphLogger.cpp
  GraphLogger.h
)

target_include_directories(simulation_graph_logger
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}          # for ton/ton-types.h
    ${CMAKE_SOURCE_DIR}/crypto   # for common/refcnt.hpp (pulled in by bitstring.h)
    # td/utils/* comes transitively from linking tdutils (PUBLIC include)
)

target_link_libraries(simulation_graph_logger
  PUBLIC
    tdutils   # hex_encode, Random, Slice
)

set_target_properties(simulation_graph_logger PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)

# ── ConsensusHarness (optional, requires full consensus build) ─────────────
if(WITH_SIMULATION)
  add_executable(ConsensusHarness
    ConsensusHarness.cpp
  )

  target_include_directories(ConsensusHarness
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_SOURCE_DIR}
  )

  target_link_libraries(ConsensusHarness
    PRIVATE
      simulation_graph_logger
      tdutils
      tdactor
      validator        # pulls in consensus/ transitively
  )

  set_target_properties(ConsensusHarness PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
endif()
