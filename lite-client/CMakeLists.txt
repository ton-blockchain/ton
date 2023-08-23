cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

add_library(lite-client-common lite-client-common.cpp lite-client-common.h)
target_link_libraries(lite-client-common PUBLIC tdutils tdactor adnllite tl_api tl_lite_api tl-lite-utils ton_crypto ton_block)

add_executable(lite-client lite-client.cpp lite-client.h)
if (NOT NIX AND NOT ANDROID)
  add_dependencies(lite-client openssl)
endif()
target_link_libraries(lite-client tdutils tdactor adnllite tl_api tl_lite_api tl-lite-utils ton_crypto ton_block
  terminal lite-client-common git ${OPENSSL_CRYPTO_LIBRARY})

target_include_directories(lite-client PRIVATE ${OPENSSL_INCLUDE_DIR})

install(TARGETS lite-client RUNTIME DESTINATION bin)
