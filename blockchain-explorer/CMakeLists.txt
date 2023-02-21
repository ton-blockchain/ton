cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

option(BUILD_SHARED_LIBS "Use \"OFF\" for a static build." ON)

set(BLOCHAIN_EXPLORER_SOURCE
    blockchain-explorer.cpp
    blockchain-explorer.hpp
    blockchain-explorer-http.cpp
    blockchain-explorer-http.hpp
    blockchain-explorer-query.cpp
    blockchain-explorer-query.hpp
)

add_executable(blockchain-explorer ${BLOCHAIN_EXPLORER_SOURCE})

message (STATUS "BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS}")

if (NOT BUILD_SHARED_LIBS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(MHD libmicrohttpd)
  target_include_directories(blockchain-explorer PUBLIC ${MHD_STATIC_INCLUDE_DIRS})
  target_link_libraries(blockchain-explorer tdutils tdactor adnllite tl_lite_api tl-lite-utils ton_crypto ton_block ${MHD_STATIC_LIBRARIES})
else()
  find_package(MHD)
  target_include_directories(blockchain-explorer PUBLIC ${MHD_INCLUDE_DIRS})
  target_link_libraries(blockchain-explorer tdutils tdactor adnllite tl_lite_api tl-lite-utils ton_crypto ton_block ${MHD_LIBRARIES})
endif()

install(TARGETS blockchain-explorer RUNTIME DESTINATION bin)

