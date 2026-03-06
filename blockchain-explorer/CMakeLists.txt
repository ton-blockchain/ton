option(NIX "Use \"ON\" for a static build." OFF)

if (USE_EMSCRIPTEN OR EMSCRIPTEN)
  return()
endif()

set(BLOCHAIN_EXPLORER_SOURCE
    blockchain-explorer.cpp
    blockchain-explorer.hpp
    blockchain-explorer-http.cpp
    blockchain-explorer-http.hpp
    blockchain-explorer-query.cpp
    blockchain-explorer-query.hpp
)

add_executable(blockchain-explorer ${BLOCHAIN_EXPLORER_SOURCE})

if (NOT MHD_FOUND AND NOT USE_EMSCRIPTEN AND NOT EMSCRIPTEN)
  message(FATAL_ERROR "libmicrohttpd not found; ensure the submodule is present and BuildMHD.cmake ran.")
endif()

target_include_directories(blockchain-explorer PUBLIC ${MHD_INCLUDE_DIR})
target_link_libraries(blockchain-explorer tdactor adnllite tl_lite_api tl-lite-utils ton_crypto ${MHD_LIBRARY})
target_link_libraries(blockchain-explorer lite-client-common)
if (TARGET mhd)
  add_dependencies(blockchain-explorer mhd)
endif()

install(TARGETS blockchain-explorer RUNTIME DESTINATION bin)
