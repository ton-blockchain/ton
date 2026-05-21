add_library(validatorsession STATIC
  candidate-serializer.cpp
  candidate-serializer.h
  validator-session-types.cpp
  validator-session-types.h
)

target_include_directories(validatorsession PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/..
  ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(validatorsession PRIVATE tdutils tdactor adnl rldp2 tl_api dht tdfec overlay)
