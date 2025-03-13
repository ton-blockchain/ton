add_executable(rldp-http-proxy rldp-http-proxy.cpp DNSResolver.h DNSResolver.cpp)
target_include_directories(rldp-http-proxy PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(rldp-http-proxy PRIVATE tonhttp rldp rldp2 dht tonlib git)

install(TARGETS rldp-http-proxy RUNTIME DESTINATION bin)
