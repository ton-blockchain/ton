add_executable(rldp-http-proxy rldp-http-proxy.cpp ProxyTransportDispatcher.h DNSResolver.h DNSResolver.cpp)
target_include_directories(rldp-http-proxy PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(rldp-http-proxy PRIVATE tonhttp rldp2 quic dht tonlib git)

install(TARGETS rldp-http-proxy RUNTIME DESTINATION bin)
