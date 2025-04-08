if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(FEC_SOURCE
  fec.h
  fec.cpp
)

add_library(fec STATIC ${FEC_SOURCE})

target_include_directories(fec PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/.. 
  ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(fec PRIVATE tl_api)
target_link_libraries(fec PUBLIC tdfec)


