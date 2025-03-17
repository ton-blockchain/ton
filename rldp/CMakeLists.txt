if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(RLDP_SOURCE
  rldp.cpp
  rldp-peer.cpp

  rldp.h
  rldp.hpp
  rldp-peer.h
  rldp-peer.hpp
)

add_library(rldp STATIC ${RLDP_SOURCE})

target_include_directories(rldp PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/.. 
  ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(rldp PRIVATE tdutils tdactor fec adnl tl_api)

