if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(VALIDATOR_SESSION_SOURCE
  candidate-serializer.cpp
  persistent-vector.cpp
  validator-session-description.cpp
  validator-session-state.cpp
  validator-session.cpp
  validator-session-round-attempt-state.cpp

  candidate-serializer.h
  persistent-vector.h
  validator-session-description.h
  validator-session-description.hpp
  validator-session-state.h
  validator-session.h
  validator-session.hpp
  validator-session-round-attempt-state.h)

add_library(validatorsession STATIC ${VALIDATOR_SESSION_SOURCE})

target_include_directories(validatorsession PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/.. 
  ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(validatorsession PRIVATE tdutils tdactor adnl rldp tl_api dht tdfec overlay catchain)
