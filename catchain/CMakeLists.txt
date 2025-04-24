if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(CATCHAIN_SOURCE
  catchain-received-block.cpp
  #catchain-receiver-fork.cpp
  catchain-receiver-source.cpp
  catchain-receiver.cpp
  catchain-block.cpp
  catchain.cpp

  catchain-block.hpp
  catchain-received-block.h
  catchain-received-block.hpp
  #catchain-receiver-fork.h
  #catchain-receiver-fork.hpp
  catchain-receiver-interface.h
  catchain-receiver-source.h
  catchain-receiver-source.hpp
  catchain-receiver.h
  catchain-receiver.hpp
  catchain-types.h
  catchain.h
  catchain.hpp
)

add_library(catchain STATIC ${CATCHAIN_SOURCE})

target_include_directories(overlay PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/.. 
  ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(catchain PRIVATE tdutils tdactor adnl tl_api dht tdfec overlay)

