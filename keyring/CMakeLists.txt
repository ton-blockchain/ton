set(KEYRING_SOURCE
  keyring.h
  keyring.hpp
  keyring.cpp
)

add_library(keyring STATIC ${KEYRING_SOURCE})

target_include_directories(keyring PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(keyring PUBLIC tdactor ton_crypto tl_api common )
