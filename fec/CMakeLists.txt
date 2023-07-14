cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

set(FEC_SOURCE
  fec.h
  fec.cpp
)

add_library(fec STATIC ${FEC_SOURCE})
add_dependencies(fec openssl)

target_include_directories(fec PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/.. 
  ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(fec PRIVATE tl_api ${OPENSSL_CRYPTO_LIBRARY})
target_link_libraries(fec PUBLIC tdfec ${OPENSSL_CRYPTO_LIBRARY})


