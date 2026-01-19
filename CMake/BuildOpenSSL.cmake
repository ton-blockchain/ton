if (NOT OPENSSL_CRYPTO_LIBRARY)

    set(OPENSSL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/openssl)
    set(OPENSSL_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/openssl)
    set(OPENSSL_INCLUDE_DIR ${OPENSSL_BINARY_DIR}/include)

    if (APPLE)
      set(CMD ./Configure darwin64-x86_64-cc --prefix=${OPENSSL_BINARY_DIR} no-shared no-dso no-engine no-unit-test no-tests enable-quic --libdir=lib)
    else()
      set(CMD ./config --prefix=${OPENSSL_BINARY_DIR} no-shared no-dso no-engine no-unit-test no-tests enable-quic --libdir=lib)
    endif()

    file(MAKE_DIRECTORY ${OPENSSL_BINARY_DIR})

    if (MSVC)
      set(OPENSSL_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/openssl)
      set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_SOURCE_DIR}/libcrypto.lib)
      set(OPENSSL_INCLUDE_DIR ${OPENSSL_BINARY_DIR}/include)
      add_custom_command(
        WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
        COMMAND perl Configure VC-WIN64A no-shared no-unit-test no-tests
        COMMAND nmake
        COMMENT "Build OpenSSL with vs2017"
        DEPENDS ${OPENSSL_SOURCE_DIR}
        OUTPUT ${OPENSSL_CRYPTO_LIBRARY}
      )
    elseif (USE_EMSCRIPTEN OR EMSCRIPTEN)
      set(OPENSSL_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/openssl)
      set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_BINARY_DIR}/libcrypto.a)
      set(OPENSSL_INCLUDE_DIR ${OPENSSL_BINARY_DIR}/include)
      add_custom_command(
          WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
          COMMAND ${CMAKE_COMMAND} -E rm -f configdata.pm Makefile
          COMMAND ${CMAKE_COMMAND} -E env
            CC=emcc
            CXX=em++
            AR=emar
            RANLIB=emranlib
            NM=llvm-nm
            ARFLAGS=rcs
            ./Configure linux-generic32 no-asm no-shared no-dso no-engine no-unit-test no-tests no-apps no-threads enable-quic
          COMMAND sed -i 's/CROSS_COMPILE=.*/CROSS_COMPILE=/g' Makefile
          COMMAND sed -i 's/-ldl//g' Makefile
          COMMAND sed -i 's/-O3/-Os/g' Makefile
          COMMAND emmake make build_libs
          COMMENT "Build OpenSSL with emscripten"
          DEPENDS ${OPENSSL_SOURCE_DIR}
          OUTPUT ${OPENSSL_CRYPTO_LIBRARY}
      )
    else()
      set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_BINARY_DIR}/lib/libcrypto.a)
      set(OPENSSL_SSL_LIBRARY ${OPENSSL_BINARY_DIR}/lib/libssl.a)
      set(OPENSSL_QUIC_CHECK_STAMP ${OPENSSL_BINARY_DIR}/.openssl_quic_checked)

      set(OPENSSL_NEEDS_BUILD FALSE)
      if (NOT EXISTS ${OPENSSL_SSL_LIBRARY} OR NOT EXISTS ${OPENSSL_CRYPTO_LIBRARY})
        set(OPENSSL_NEEDS_BUILD TRUE)
      else()
        set(OPENSSL_NM_EXECUTABLE ${CMAKE_NM})
        if (NOT OPENSSL_NM_EXECUTABLE)
          find_program(OPENSSL_NM_EXECUTABLE nm)
        endif()

        if (NOT OPENSSL_NM_EXECUTABLE)
          set(OPENSSL_NEEDS_BUILD TRUE)
        else()
          execute_process(
            COMMAND ${OPENSSL_NM_EXECUTABLE} -g ${OPENSSL_SSL_LIBRARY}
            RESULT_VARIABLE OPENSSL_NM_RESULT
            OUTPUT_VARIABLE OPENSSL_NM_OUTPUT
            ERROR_VARIABLE OPENSSL_NM_ERROR
          )
          if (NOT OPENSSL_NM_RESULT EQUAL 0)
            set(OPENSSL_NEEDS_BUILD TRUE)
          else()
            string(FIND "${OPENSSL_NM_OUTPUT}" "SSL_provide_quic_data" HAVE_SSL_PROVIDE_QUIC_DATA)
            string(FIND "${OPENSSL_NM_OUTPUT}" "SSL_set_quic_tls_cbs" HAVE_SSL_SET_QUIC_TLS_CBS)
            if (HAVE_SSL_PROVIDE_QUIC_DATA EQUAL -1 AND HAVE_SSL_SET_QUIC_TLS_CBS EQUAL -1)
              set(OPENSSL_NEEDS_BUILD TRUE)
            endif()
          endif()
        endif()
      endif()

      if (OPENSSL_NEEDS_BUILD)
        execute_process(
          COMMAND ${CMD}
          WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
          RESULT_VARIABLE OPENSSL_CONFIG_RESULT
        )
        if (NOT OPENSSL_CONFIG_RESULT EQUAL 0)
          message(FATAL_ERROR "OpenSSL config failed with code ${OPENSSL_CONFIG_RESULT}")
        endif()
        execute_process(
          COMMAND make -j16
          WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
          RESULT_VARIABLE OPENSSL_MAKE_RESULT
        )
        if (NOT OPENSSL_MAKE_RESULT EQUAL 0)
          message(FATAL_ERROR "OpenSSL build failed with code ${OPENSSL_MAKE_RESULT}")
        endif()
        execute_process(
          COMMAND make install_sw
          WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
          RESULT_VARIABLE OPENSSL_INSTALL_RESULT
        )
        if (NOT OPENSSL_INSTALL_RESULT EQUAL 0)
          message(FATAL_ERROR "OpenSSL install failed with code ${OPENSSL_INSTALL_RESULT}")
        endif()
      endif()

      add_custom_command(
          WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
          COMMAND ${CMD}
          COMMAND make -j16
          COMMAND make install_sw
          COMMENT "Build OpenSSL with QUIC support"
          DEPENDS ${OPENSSL_SOURCE_DIR}
          OUTPUT ${OPENSSL_CRYPTO_LIBRARY} ${OPENSSL_SSL_LIBRARY}
      )
      add_custom_command(
          COMMAND ${CMAKE_COMMAND}
            -DOPENSSL_SSL_LIBRARY=${OPENSSL_SSL_LIBRARY}
            -DOPENSSL_NM=${CMAKE_NM}
            -DOPENSSL_QUIC_STAMP=${OPENSSL_QUIC_CHECK_STAMP}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/CheckOpenSSLQuic.cmake
          COMMENT "Verify OpenSSL QUIC symbols"
          DEPENDS ${OPENSSL_SSL_LIBRARY}
          OUTPUT ${OPENSSL_QUIC_CHECK_STAMP}
      )
    endif()

else()
   message(STATUS "Use openssl: ${OPENSSL_CRYPTO_LIBRARY}")
endif()

set(OPENSSL_DEPS ${OPENSSL_CRYPTO_LIBRARY})
if (DEFINED OPENSSL_SSL_LIBRARY)
  list(APPEND OPENSSL_DEPS ${OPENSSL_SSL_LIBRARY})
endif()
if (DEFINED OPENSSL_QUIC_CHECK_STAMP)
  list(APPEND OPENSSL_DEPS ${OPENSSL_QUIC_CHECK_STAMP})
endif()
add_custom_target(OpenSSL DEPENDS ${OPENSSL_DEPS})
