include(AndroidThirdParty)

if (NOT OPENSSL_CRYPTO_LIBRARY)

    set(OPENSSL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/openssl)
    if (ANDROID)
      set(OPENSSL_INSTALL_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/crypto/${TON_ANDROID_OPENSSL_DIR})
      set(OPENSSL_BINARY_DIR ${OPENSSL_INSTALL_DIR})
      set(OPENSSL_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/openssl-android-${TON_ANDROID_OPENSSL_DIR})
      set(OPENSSL_BUILD_ROOT ${CMAKE_CURRENT_BINARY_DIR}/third-party)
      set(OPENSSL_CONFIGURE_SCRIPT ${OPENSSL_BUILD_DIR}/Configure)
      set(OPENSSL_INCLUDE_DIR ${OPENSSL_INSTALL_DIR}/include)
      set(OPENSSL_ANDROID_ENV
        ${CMAKE_COMMAND} -E env
        ANDROID_NDK_ROOT=${TON_ANDROID_NDK_ROOT}
        ANDROID_NDK_HOME=${TON_ANDROID_NDK_ROOT}
        ANDROID_NDK=${TON_ANDROID_NDK_ROOT}
        PERL=/usr/bin/perl
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${TON_ANDROID_AR}
        RANLIB=${TON_ANDROID_RANLIB}
        NM=${TON_ANDROID_NM}
        PATH=${TON_ANDROID_NDK_BIN}:$ENV{PATH}
      )
      set(OPENSSL_ANDROID_MAKE_ARGS
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${TON_ANDROID_AR}
        RANLIB=${TON_ANDROID_RANLIB}
        NM=${TON_ANDROID_NM}
      )
      if (TON_ANDROID_ARCH STREQUAL "arm")
        set(OPENSSL_CONFIGURE_TARGET android-arm)
      elseif (TON_ANDROID_ARCH STREQUAL "arm64")
        set(OPENSSL_CONFIGURE_TARGET android-arm64)
      elseif (TON_ANDROID_ARCH STREQUAL "x86")
        set(OPENSSL_CONFIGURE_TARGET android-x86)
      elseif (TON_ANDROID_ARCH STREQUAL "x86_64")
        set(OPENSSL_CONFIGURE_TARGET android-x86_64)
      else()
        message(FATAL_ERROR "Unsupported Android arch for OpenSSL: ${TON_ANDROID_ARCH}")
      endif()
      set(CMD ${OPENSSL_ANDROID_ENV}
        /usr/bin/perl ./Configure ${OPENSSL_CONFIGURE_TARGET} --prefix=${OPENSSL_INSTALL_DIR}
          no-shared no-dso no-engine no-unit-test no-tests no-apps enable-quic --libdir=lib -D__ANDROID_API__=${TON_ANDROID_API})
    else()
      set(OPENSSL_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/openssl)
      set(OPENSSL_INCLUDE_DIR ${OPENSSL_BINARY_DIR}/include)
      if (APPLE)
        set(CMD ./Configure darwin64-x86_64-cc --prefix=${OPENSSL_BINARY_DIR} no-shared no-dso no-engine no-unit-test no-tests enable-quic --libdir=lib)
      else()
        set(CMD ./config --prefix=${OPENSSL_BINARY_DIR} no-shared no-dso no-engine no-unit-test no-tests enable-quic --libdir=lib)
      endif()
    endif()

    file(MAKE_DIRECTORY ${OPENSSL_BINARY_DIR})
    if (ANDROID)
      file(MAKE_DIRECTORY ${OPENSSL_BUILD_DIR})
      file(MAKE_DIRECTORY ${OPENSSL_BUILD_ROOT})
      if (NOT EXISTS "${OPENSSL_SOURCE_DIR}/Configure")
        message(FATAL_ERROR "OpenSSL source missing Configure at ${OPENSSL_SOURCE_DIR}/Configure")
      endif()
    endif()

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
      if (ANDROID)
      set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_BUILD_DIR}/libcrypto.a)
      set(OPENSSL_SSL_LIBRARY ${OPENSSL_BUILD_DIR}/libssl.a)
      set(OPENSSL_QUIC_CHECK_STAMP ${OPENSSL_BUILD_DIR}/.openssl_quic_checked)
      else()
        set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_BINARY_DIR}/lib/libcrypto.a)
        set(OPENSSL_SSL_LIBRARY ${OPENSSL_BINARY_DIR}/lib/libssl.a)
        set(OPENSSL_QUIC_CHECK_STAMP ${OPENSSL_BINARY_DIR}/.openssl_quic_checked)
      endif()

      if (ANDROID)
        set(OPENSSL_NEEDS_BUILD FALSE)
        set(OPENSSL_ARCH_STAMP ${OPENSSL_BUILD_DIR}/.android_arch)
        set(OPENSSL_ARCH_EXPECTED "${TON_ANDROID_ABI};${TON_ANDROID_API};${TON_ANDROID_CC}")
        if (EXISTS ${OPENSSL_ARCH_STAMP})
          file(READ ${OPENSSL_ARCH_STAMP} OPENSSL_ARCH_CURRENT)
          string(STRIP "${OPENSSL_ARCH_CURRENT}" OPENSSL_ARCH_CURRENT)
          if (NOT OPENSSL_ARCH_CURRENT STREQUAL OPENSSL_ARCH_EXPECTED)
            set(OPENSSL_NEEDS_BUILD TRUE)
          endif()
        else()
          set(OPENSSL_NEEDS_BUILD TRUE)
        endif()
        if (NOT EXISTS ${OPENSSL_SSL_LIBRARY} OR NOT EXISTS ${OPENSSL_CRYPTO_LIBRARY})
          set(OPENSSL_NEEDS_BUILD TRUE)
        endif()
        if (OPENSSL_NEEDS_BUILD)
          file(REMOVE_RECURSE ${OPENSSL_BUILD_DIR})
          file(MAKE_DIRECTORY ${OPENSSL_BUILD_DIR})
          # Clean OpenSSL source directory before copying to avoid architecture conflicts
          execute_process(
            COMMAND make clean
            WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
            RESULT_VARIABLE OPENSSL_CLEAN_RESULT
            OUTPUT_QUIET
            ERROR_QUIET
          )
          file(COPY ${OPENSSL_SOURCE_DIR}/ DESTINATION ${OPENSSL_BUILD_DIR})
          if (NOT EXISTS ${OPENSSL_CONFIGURE_SCRIPT})
            message(FATAL_ERROR "OpenSSL Configure script missing at ${OPENSSL_CONFIGURE_SCRIPT}")
          endif()
          message(STATUS "OpenSSL source dir: ${OPENSSL_SOURCE_DIR}")
          message(STATUS "OpenSSL build dir: ${OPENSSL_BUILD_DIR}")
          execute_process(
            COMMAND ${CMAKE_COMMAND} -E echo "OpenSSL configure cmd: ${CMD}"
            COMMAND ${CMD}
            WORKING_DIRECTORY ${OPENSSL_BUILD_DIR}
            RESULT_VARIABLE OPENSSL_CONFIG_RESULT
            OUTPUT_VARIABLE OPENSSL_CONFIG_OUTPUT
            ERROR_VARIABLE OPENSSL_CONFIG_ERROR
          )
          if (NOT OPENSSL_CONFIG_RESULT EQUAL 0)
            message(STATUS "OpenSSL config output: ${OPENSSL_CONFIG_OUTPUT}")
            message(STATUS "OpenSSL config error: ${OPENSSL_CONFIG_ERROR}")
            message(FATAL_ERROR "OpenSSL config failed with code ${OPENSSL_CONFIG_RESULT} in ${OPENSSL_BUILD_DIR}")
          endif()
          file(WRITE ${OPENSSL_ARCH_STAMP} "${OPENSSL_ARCH_EXPECTED}\n")
          execute_process(
            COMMAND ${OPENSSL_ANDROID_ENV} make -j16 -C ${OPENSSL_BUILD_DIR} ${OPENSSL_ANDROID_MAKE_ARGS}
            WORKING_DIRECTORY ${OPENSSL_BUILD_ROOT}
            RESULT_VARIABLE OPENSSL_MAKE_RESULT
          )
          if (NOT OPENSSL_MAKE_RESULT EQUAL 0)
            message(FATAL_ERROR "OpenSSL build failed with code ${OPENSSL_MAKE_RESULT}")
          endif()
          execute_process(
            COMMAND ${OPENSSL_ANDROID_ENV} make -j16 -C ${OPENSSL_BUILD_DIR} ${OPENSSL_ANDROID_MAKE_ARGS} install_sw
            WORKING_DIRECTORY ${OPENSSL_BUILD_ROOT}
            RESULT_VARIABLE OPENSSL_INSTALL_RESULT
          )
          if (NOT OPENSSL_INSTALL_RESULT EQUAL 0)
            message(FATAL_ERROR "OpenSSL install failed with code ${OPENSSL_INSTALL_RESULT}")
          endif()
        endif()

        add_custom_command(
            WORKING_DIRECTORY ${OPENSSL_BUILD_DIR}
            COMMAND ${CMD}
            COMMAND ${OPENSSL_ANDROID_ENV} make -j16 -C ${OPENSSL_BUILD_DIR} ${OPENSSL_ANDROID_MAKE_ARGS}
            COMMAND ${OPENSSL_ANDROID_ENV} make -j16 -C ${OPENSSL_BUILD_DIR} ${OPENSSL_ANDROID_MAKE_ARGS} install_sw
            COMMENT "Build OpenSSL with QUIC support (Android)"
            DEPENDS ${OPENSSL_SOURCE_DIR}
            OUTPUT ${OPENSSL_CRYPTO_LIBRARY} ${OPENSSL_SSL_LIBRARY}
        )
      else()
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
      endif()
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
