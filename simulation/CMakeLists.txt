cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

# simulation/CMakeLists.txt
# Builds GraphLogger as a static library so consensus code can link it.
# The ConsensusHarness executable is only built when WITH_SIMULATION=ON.

add_library(simulation_graph_logger STATIC
  GraphLogger.cpp
  GraphLogger.h
)

if (MSVC)
  set_target_properties(simulation_graph_logger PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
  target_compile_options(simulation_graph_logger PRIVATE /DTD_WINDOWS=1 /EHsc /bigobj)
endif()

target_include_directories(simulation_graph_logger
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(simulation_graph_logger
  PUBLIC
    $<$<BOOL:${WIN32}>:bcrypt>
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
  )

  if (MSVC)
    set_target_properties(ConsensusHarness PROPERTIES
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(ConsensusHarness PRIVATE /DTD_WINDOWS=1 /EHsc /bigobj)
  endif()

  set_target_properties(ConsensusHarness PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
endif()
