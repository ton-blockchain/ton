cmake_minimum_required(VERSION 3.5 FATAL_ERROR)

option(NIX "Use \"ON\" for a static build." OFF)

set(BLOCHAIN_EXPLORER_SOURCE
    blockchain-explorer.cpp
    blockchain-explorer.hpp
    blockchain-explorer-http.cpp
    blockchain-explorer-http.hpp
    blockchain-explorer-query.cpp
    blockchain-explorer-query.hpp
)

add_executable(blockchain-explorer ${BLOCHAIN_EXPLORER_SOURCE})

if (NOT MHD_FOUND)
  find_package(PkgConfig REQUIRED)
  find_package(MHD REQUIRED)
endif()

target_include_directories(blockchain-explorer PUBLIC ${MHD_INCLUDE_DIR})
target_link_libraries(blockchain-explorer tdutils tdactor adnllite tl_lite_api tl-lite-utils ton_crypto ton_block ${MHD_LIBRARIES})

install(TARGETS blockchain-explorer RUNTIME DESTINATION bin)

