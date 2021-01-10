cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

#BEGIN internal
if (NOT TON_ONLY_TONLIB)
set(ADNL_HEADERS
  adnl-address-list.h
  adnl-address-list.hpp
  adnl-db.h
  adnl-db.hpp
  adnl-channel.h
  adnl-channel.hpp
  adnl-ext-client.h
  adnl-ext-client.hpp
  adnl-ext-connection.hpp
  adnl-ext-server.h
  adnl-ext-server.hpp
  adnl-local-id.h
  adnl-message.h
  adnl-network-manager.h
  adnl-network-manager.hpp
  adnl-node.h
  adnl-packet.h
  adnl-peer-table.h
  adnl-peer-table.hpp
  adnl-peer.h
  adnl-peer.hpp
  adnl-query.h
  adnl-static-nodes.h
  adnl-static-nodes.hpp
  adnl-proxy-types.h
  adnl-proxy-types.hpp
  adnl.h
  utils.hpp
)
set(ADNL_SOURCE
  adnl-address-list.cpp
  adnl-db.cpp
  adnl-ext-client.cpp
  adnl-ext-server.cpp
  adnl-ext-connection.cpp
  adnl-local-id.cpp
  adnl-message.cpp
  adnl-network-manager.cpp
  adnl-node.cpp
  adnl-node-id.cpp
  adnl-packet.cpp
  adnl-peer-table.cpp
  adnl-peer.cpp
  adnl-query.cpp
  adnl-channel.cpp
  adnl-static-nodes.cpp
  adnl-proxy-types.cpp
  utils.cpp
  ${ADNL_HEADERS}
)

set(ADNL_TEST_SOURCE
  adnl-test-loopback-implementation.h
  adnl-test-loopback-implementation.cpp
)

set(ADNL_PROXY_SOURCE
  adnl-proxy.cpp
  adnl-proxy-types.h
  adnl-proxy-types.hpp
  adnl-proxy-types.cpp
)

#FIXME
set(ADNL_LITE_HEADERS ${ADNL_HEADERS})
endif()
#END internal

set(ADNL_LITE_SOURCE
  adnl-ext-client.cpp
  adnl-ext-connection.cpp
  adnl-query.cpp

  ${ADNL_LITE_HEADERS}
)

#BEGIN internal
if (NOT TON_ONLY_TONLIB)
add_library(adnl STATIC ${ADNL_SOURCE})

target_include_directories(adnl PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(adnl PUBLIC tdactor ton_crypto tl_api tdnet tddb keys keyring )

add_executable(adnl-proxy ${ADNL_PROXY_SOURCE})
target_include_directories(adnl-proxy PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(adnl-proxy PUBLIC tdactor ton_crypto tl_api tdnet common
  tl-utils)

add_executable(adnl-pong adnl-pong.cpp)
target_include_directories(adnl-pong PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(adnl-pong PUBLIC tdactor ton_crypto tl_api tdnet common
  tl-utils adnl dht)

add_library(adnltest STATIC ${ADNL_TEST_SOURCE})
target_include_directories(adnltest PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(adnltest PUBLIC adnl )
endif()
#END internal

add_library(adnllite STATIC ${ADNL_LITE_SOURCE})

target_include_directories(adnllite PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(adnllite PUBLIC tdactor ton_crypto tl_lite_api tdnet keys )
