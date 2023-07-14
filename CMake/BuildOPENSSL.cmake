cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_CRYPTO_LIBRARY)

    set(OPENSSL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/openssl)
    set(OPENSSL_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/openssl)
    set(OPENSSL_INCLUDE_DIR ${OPENSSL_BINARY_DIR}/include)

    file(MAKE_DIRECTORY ${OPENSSL_BINARY_DIR})

    if (MSVC)
      set(OPENSSL_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/openssl)
      set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_SOURCE_DIR}/libcrypto.lib)
      set(OPENSSL_INCLUDE_DIR ${OPENSSL_BINARY_DIR}/include)
      add_custom_command(
        WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
        COMMAND C:/Strawberry/perl/bin/perl.exe Configure VC-WIN64A no-shared no-unit-test no-tests
        COMMAND nmake
        COMMENT "Build openssl with vs2017"
        DEPENDS ${OPENSSL_SOURCE_DIR}
        OUTPUT ${OPENSSL_CRYPTO_LIBRARY}
      )
    elseif (EMSCRIPTEN)
      set(OPENSSL_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/openssl)
      set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_BINARY_DIR}/libcrypto.a)
      set(OPENSSL_INCLUDE_DIR ${OPENSSL_BINARY_DIR}/include)
      add_custom_command(
          WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
          COMMAND emconfigure ./Configure linux-generic32 no-shared no-dso no-engine no-unit-test no-ui no-tests
          COMMAND sed -i 's/CROSS_COMPILE=.*/CROSS_COMPILE=/g' Makefile
          COMMAND sed -i 's/-ldl//g' Makefile
          COMMAND sed -i 's/-O3/-Os/g' Makefile
          COMMAND emmake make depend
          COMMENT "Build openssl with emscripten"
          DEPENDS ${OPENSSL_SOURCE_DIR}
          OUTPUT ${OPENSSL_CRYPTO_LIBRARY}
      )
    else()
      set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_BINARY_DIR}/lib/libcrypto.a)
      add_custom_command(
          WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
          COMMAND ./config --prefix=${OPENSSL_BINARY_DIR} no-shared no-dso no-engine no-unit-test no-ui no-tests
          COMMAND make -j16
          COMMAND make install_sw
          COMMENT "Build openssl"
          DEPENDS ${OPENSSL_SOURCE_DIR}
          OUTPUT ${OPENSSL_CRYPTO_LIBRARY}
      )
    endif()

else()
   message(STATUS "Use openssl: ${OPENSSL_CRYPTO_LIBRARY}")
endif()

add_custom_target(openssl DEPENDS ${OPENSSL_CRYPTO_LIBRARY})
