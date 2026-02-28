include(AndroidThirdParty)

get_filename_component(TON_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TON_THIRD_PARTY_SOURCE_DIR "${TON_SOURCE_DIR}/third-party")
set(TON_THIRD_PARTY_BINARY_DIR "${CMAKE_BINARY_DIR}/third-party")

if (NOT OPENSSL_CRYPTO_LIBRARY)
  set(OPENSSL_SOURCE_DIR ${TON_THIRD_PARTY_SOURCE_DIR}/openssl)

  if (ANDROID)
    set(OPENSSL_INSTALL_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/crypto/${TON_ANDROID_OPENSSL_DIR})
    set(OPENSSL_BINARY_DIR ${OPENSSL_INSTALL_DIR})
    set(OPENSSL_BUILD_DIR ${TON_THIRD_PARTY_BINARY_DIR}/openssl-android-${TON_ANDROID_OPENSSL_DIR})
    set(OPENSSL_BUILD_ROOT ${TON_THIRD_PARTY_BINARY_DIR})
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
      no-shared no-dso no-unit-test no-tests no-apps enable-quic --libdir=lib -D__ANDROID_API__=${TON_ANDROID_API})
  else()
    set(OPENSSL_BINARY_DIR ${TON_THIRD_PARTY_BINARY_DIR}/openssl)
    set(OPENSSL_BUILD_DIR ${TON_THIRD_PARTY_BINARY_DIR}/openssl-src)
    set(OPENSSL_INCLUDE_DIR ${OPENSSL_BINARY_DIR}/include)
    if (APPLE)
      execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE MACOS_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      if (MACOS_ARCH STREQUAL "arm64")
        set(OPENSSL_DARWIN_TARGET darwin64-arm64-cc)
      else()
        set(OPENSSL_DARWIN_TARGET darwin64-x86_64-cc)
      endif()
      set(CMD ./Configure ${OPENSSL_DARWIN_TARGET} --prefix=${OPENSSL_BINARY_DIR} no-shared no-dso no-unit-test no-tests no-apps enable-quic --libdir=lib)
    elseif (MINGW)
      set(OPENSSL_MINGW_CFLAGS "-DSIO_UDP_NETRESET=SIO_UDP_CONNRESET")
      if ("$ENV{MSYSTEM}" STREQUAL "UCRT64")
        set(OPENSSL_MINGW_CFLAGS "${OPENSSL_MINGW_CFLAGS} -D__USE_MINGW_ANSI_STDIO=1")
      endif()
      set(OPENSSL_CC "${CMAKE_C_COMPILER}")
      set(OPENSSL_CXX "${CMAKE_CXX_COMPILER}")
      set(OPENSSL_AR "${CMAKE_AR}")
      set(OPENSSL_RANLIB "${CMAKE_RANLIB}")
      if (NOT OPENSSL_CC)
        set(OPENSSL_CC clang)
      endif()
      if (NOT OPENSSL_CXX)
        set(OPENSSL_CXX clang++)
      endif()
      if (NOT OPENSSL_AR)
        set(OPENSSL_AR llvm-ar)
      endif()
      if (NOT OPENSSL_RANLIB)
        set(OPENSSL_RANLIB llvm-ranlib)
      endif()
      set(CMD ${CMAKE_COMMAND} -E env
        CC=${OPENSSL_CC}
        CXX=${OPENSSL_CXX}
        AR=${OPENSSL_AR}
        RANLIB=${OPENSSL_RANLIB}
        CFLAGS=${OPENSSL_MINGW_CFLAGS}
        perl ./Configure mingw64 --prefix=${OPENSSL_BINARY_DIR} no-shared no-dso no-unit-test no-tests no-apps enable-quic --libdir=lib)
    else()
      set(CMD ./config --prefix=${OPENSSL_BINARY_DIR} no-shared no-dso no-unit-test no-tests enable-quic --libdir=lib)
    endif()
  endif()

  file(MAKE_DIRECTORY ${OPENSSL_BINARY_DIR})
  if (NOT USE_EMSCRIPTEN AND NOT EMSCRIPTEN)
    file(MAKE_DIRECTORY ${OPENSSL_BUILD_DIR})
  endif()
  if (ANDROID)
    file(MAKE_DIRECTORY ${OPENSSL_BUILD_ROOT})
    if (NOT EXISTS "${OPENSSL_SOURCE_DIR}/Configure")
      message(FATAL_ERROR "OpenSSL source missing Configure at ${OPENSSL_SOURCE_DIR}/Configure")
    endif()
  endif()

  if (MSVC)
    set(OPENSSL_BINARY_DIR ${TON_THIRD_PARTY_BINARY_DIR}/openssl)
    set(OPENSSL_BUILD_DIR ${TON_THIRD_PARTY_BINARY_DIR}/openssl-src-msvc)
    set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_BINARY_DIR}/lib/libcrypto.lib)
    set(OPENSSL_SSL_LIBRARY ${OPENSSL_BINARY_DIR}/lib/libssl.lib)
    set(OPENSSL_INCLUDE_DIR ${OPENSSL_BINARY_DIR}/include)
    set(OPENSSL_MSVC_ENV ${CMAKE_COMMAND} -E env CL=/FS CFLAGS=/FS CXXFLAGS=/FS)
    if (NOT EXISTS "${OPENSSL_CRYPTO_LIBRARY}" OR NOT EXISTS "${OPENSSL_SSL_LIBRARY}")
      file(REMOVE_RECURSE ${OPENSSL_BUILD_DIR})
      file(COPY ${OPENSSL_SOURCE_DIR}/ DESTINATION ${OPENSSL_BUILD_DIR})
      execute_process(
        COMMAND ${OPENSSL_MSVC_ENV} perl Configure VC-WIN64A --prefix=${OPENSSL_BINARY_DIR} --openssldir=${OPENSSL_BINARY_DIR} no-shared no-unit-test no-tests no-apps enable-quic
        WORKING_DIRECTORY ${OPENSSL_BUILD_DIR}
        RESULT_VARIABLE OPENSSL_CONFIG_RESULT
      )
      if (NOT OPENSSL_CONFIG_RESULT EQUAL 0)
        message(FATAL_ERROR "OpenSSL config failed with code ${OPENSSL_CONFIG_RESULT}")
      endif()
      execute_process(
        COMMAND ${OPENSSL_MSVC_ENV} nmake
        WORKING_DIRECTORY ${OPENSSL_BUILD_DIR}
        RESULT_VARIABLE OPENSSL_MAKE_RESULT
      )
      if (NOT OPENSSL_MAKE_RESULT EQUAL 0)
        message(FATAL_ERROR "OpenSSL build failed with code ${OPENSSL_MAKE_RESULT}")
      endif()
      execute_process(
        COMMAND ${OPENSSL_MSVC_ENV} nmake install_sw
        WORKING_DIRECTORY ${OPENSSL_BUILD_DIR}
        RESULT_VARIABLE OPENSSL_INSTALL_RESULT
      )
      if (NOT OPENSSL_INSTALL_RESULT EQUAL 0)
        message(FATAL_ERROR "OpenSSL install failed with code ${OPENSSL_INSTALL_RESULT}")
      endif()
    endif()
    add_custom_command(
      COMMAND ${CMAKE_COMMAND} -E touch ${OPENSSL_CRYPTO_LIBRARY}
      COMMAND ${CMAKE_COMMAND} -E touch ${OPENSSL_SSL_LIBRARY}
      COMMENT "OpenSSL already built during configuration (MSVC)"
      OUTPUT ${OPENSSL_CRYPTO_LIBRARY} ${OPENSSL_SSL_LIBRARY}
    )
  elseif (USE_EMSCRIPTEN OR EMSCRIPTEN)
    set(OPENSSL_BINARY_DIR ${TON_THIRD_PARTY_BINARY_DIR}/openssl-emscripten)
    set(OPENSSL_BUILD_DIR ${TON_THIRD_PARTY_BINARY_DIR}/openssl-src-emscripten)
    set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_BINARY_DIR}/libcrypto.a)
    set(OPENSSL_INCLUDE_DIR ${OPENSSL_BUILD_DIR}/include)
    add_custom_command(
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${OPENSSL_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${OPENSSL_SOURCE_DIR} ${OPENSSL_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${OPENSSL_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR} ${CMAKE_COMMAND} -E rm -f configdata.pm Makefile
      COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=emcc
        CXX=em++
        AR=emar
        RANLIB=emranlib
        NM=llvm-nm
        ARFLAGS=rcs
        ./Configure linux-generic32 --prefix=${OPENSSL_BINARY_DIR} no-asm no-shared no-dso no-unit-test no-tests no-apps no-threads enable-quic
      COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR} sed -i "s/CROSS_COMPILE=.*/CROSS_COMPILE=/g" Makefile
      COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR} sed -i "s/-ldl//g" Makefile
      COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR} sed -i "s/-O3/-Os/g" Makefile
      COMMAND ${CMAKE_COMMAND} -E chdir ${OPENSSL_BUILD_DIR} emmake make build_libs
      COMMAND ${CMAKE_COMMAND} -E copy ${OPENSSL_BUILD_DIR}/libcrypto.a ${OPENSSL_CRYPTO_LIBRARY}
      COMMENT "Build OpenSSL with emscripten"
      DEPENDS ${OPENSSL_SOURCE_DIR}/Configure
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
    if (MINGW)
      set(OPENSSL_LIBRARIES ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY}
        ws2_32 bcrypt crypt32 advapi32 user32 gdi32 kernel32)
    else()
      set(OPENSSL_LIBRARIES ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY})
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
        file(COPY ${OPENSSL_SOURCE_DIR}/ DESTINATION ${OPENSSL_BUILD_DIR})
        if (NOT EXISTS ${OPENSSL_CONFIGURE_SCRIPT})
          message(FATAL_ERROR "OpenSSL Configure script missing at ${OPENSSL_CONFIGURE_SCRIPT}")
        endif()
        execute_process(
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
        if (MINGW)
          if (NOT EXISTS "${OPENSSL_BUILD_DIR}/configdata.pm")
            set(OPENSSL_NEEDS_BUILD TRUE)
          else()
            file(READ "${OPENSSL_BUILD_DIR}/configdata.pm" OPENSSL_CONFIGDATA)
            string(FIND "${OPENSSL_CONFIGDATA}" "no-threads" OPENSSL_NO_THREADS_POS)
            if (OPENSSL_NO_THREADS_POS EQUAL -1)
              set(OPENSSL_NEEDS_BUILD TRUE)
            endif()
            if ("$ENV{MSYSTEM}" STREQUAL "UCRT64")
              string(FIND "${OPENSSL_CONFIGDATA}" "__USE_MINGW_ANSI_STDIO" OPENSSL_UCRT_STDIO_POS)
              if (OPENSSL_UCRT_STDIO_POS EQUAL -1)
                set(OPENSSL_NEEDS_BUILD TRUE)
              endif()
            endif()
          endif()
        endif()
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
            if (MINGW)
              execute_process(
                COMMAND ${OPENSSL_NM_EXECUTABLE} -g ${OPENSSL_CRYPTO_LIBRARY}
                RESULT_VARIABLE OPENSSL_CRYPTO_NM_RESULT
                OUTPUT_VARIABLE OPENSSL_CRYPTO_NM_OUTPUT
                ERROR_VARIABLE OPENSSL_CRYPTO_NM_ERROR
              )
              if (NOT OPENSSL_CRYPTO_NM_RESULT EQUAL 0)
                set(OPENSSL_NEEDS_BUILD TRUE)
              else()
                string(FIND "${OPENSSL_CRYPTO_NM_OUTPUT}" "__security_cookie" OPENSSL_CRYPTO_HAS_SECURITY_COOKIE)
                string(FIND "${OPENSSL_CRYPTO_NM_OUTPUT}" "__GSHandlerCheck" OPENSSL_CRYPTO_HAS_GS_HANDLER)
                if (NOT OPENSSL_CRYPTO_HAS_SECURITY_COOKIE EQUAL -1 OR NOT OPENSSL_CRYPTO_HAS_GS_HANDLER EQUAL -1)
                  set(OPENSSL_NEEDS_BUILD TRUE)
                endif()
              endif()
            endif()
          endif()
        endif()
      endif()

      if (OPENSSL_NEEDS_BUILD)
        file(REMOVE_RECURSE ${OPENSSL_BUILD_DIR})
        file(MAKE_DIRECTORY ${OPENSSL_BUILD_DIR})
        file(COPY ${OPENSSL_SOURCE_DIR}/ DESTINATION ${OPENSSL_BUILD_DIR})
        execute_process(
          COMMAND ${CMD}
          WORKING_DIRECTORY ${OPENSSL_BUILD_DIR}
          RESULT_VARIABLE OPENSSL_CONFIG_RESULT
        )
        if (NOT OPENSSL_CONFIG_RESULT EQUAL 0)
          message(FATAL_ERROR "OpenSSL config failed with code ${OPENSSL_CONFIG_RESULT}")
        endif()
        execute_process(
          COMMAND make -j16
          WORKING_DIRECTORY ${OPENSSL_BUILD_DIR}
          RESULT_VARIABLE OPENSSL_MAKE_RESULT
        )
        if (NOT OPENSSL_MAKE_RESULT EQUAL 0)
          message(FATAL_ERROR "OpenSSL build failed with code ${OPENSSL_MAKE_RESULT}")
        endif()
        execute_process(
          COMMAND make install_sw
          WORKING_DIRECTORY ${OPENSSL_BUILD_DIR}
          RESULT_VARIABLE OPENSSL_INSTALL_RESULT
        )
        if (NOT OPENSSL_INSTALL_RESULT EQUAL 0)
          message(FATAL_ERROR "OpenSSL install failed with code ${OPENSSL_INSTALL_RESULT}")
        endif()
      endif()

      add_custom_command(
        COMMAND ${CMAKE_COMMAND} -E touch ${OPENSSL_CRYPTO_LIBRARY}
        COMMAND ${CMAKE_COMMAND} -E touch ${OPENSSL_SSL_LIBRARY}
        COMMENT "OpenSSL already built during configuration"
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
