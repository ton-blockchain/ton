include(AndroidThirdParty)

if (NOT SECP256K1_LIBRARY)
  set(SECP256K1_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/secp256k1)
  set(SECP256K1_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/secp256k1)
  set(SECP256K1_BUILD_DIR ${SECP256K1_BINARY_DIR}/src)
  set(SECP256K1_INCLUDE_DIR ${SECP256K1_BINARY_DIR}/include)

  file(MAKE_DIRECTORY ${SECP256K1_BINARY_DIR})
  file(MAKE_DIRECTORY ${SECP256K1_BUILD_DIR})
  file(MAKE_DIRECTORY "${SECP256K1_BINARY_DIR}/include")

  if (MSVC)
    set(SECP256K1_BUILD_DIR ${SECP256K1_BINARY_DIR}/cmake-build)
    set(SECP256K1_LIBRARY ${SECP256K1_BINARY_DIR}/lib/libsecp256k1.lib)
    set(SECP256K1_INCLUDE_DIR ${SECP256K1_SOURCE_DIR}/include)
    file(MAKE_DIRECTORY ${SECP256K1_BINARY_DIR}/lib)
    add_custom_command(
      WORKING_DIRECTORY ${SECP256K1_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${SECP256K1_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${SECP256K1_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E env CFLAGS=/WX cmake -S ${SECP256K1_SOURCE_DIR} -B ${SECP256K1_BUILD_DIR} -A x64 -DSECP256K1_ENABLE_MODULE_RECOVERY=ON -DSECP256K1_ENABLE_MODULE_EXTRAKEYS=ON -DSECP256K1_BUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=${SECP256K1_BINARY_DIR}/lib -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE=${SECP256K1_BINARY_DIR}/lib
      COMMAND ${CMAKE_COMMAND} --build ${SECP256K1_BUILD_DIR} --config Release
      COMMENT "Build Secp256k1"
      DEPENDS ${SECP256K1_SOURCE_DIR}
      OUTPUT ${SECP256K1_LIBRARY}
    )
  elseif (EMSCRIPTEN)
    set(SECP256K1_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/secp256k1-emscripten)
    set(SECP256K1_EMSRC_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/secp256k1-src-emscripten)
    set(SECP256K1_LIBRARY ${SECP256K1_BINARY_DIR}/lib/libsecp256k1.a)
    set(SECP256K1_INCLUDE_DIR ${SECP256K1_BINARY_DIR}/include)
    set(SECP256K1_LIBRARY ${SECP256K1_LIBRARY} CACHE FILEPATH "Secp256k1 library" FORCE)
    set(SECP256K1_INCLUDE_DIR ${SECP256K1_INCLUDE_DIR} CACHE PATH "Secp256k1 include dir" FORCE)
    if (CMAKE_AR)
      set(SECP256K1_AR ${CMAKE_AR})
    else()
      set(SECP256K1_AR emar)
    endif()
    if (CMAKE_RANLIB)
      set(SECP256K1_RANLIB ${CMAKE_RANLIB})
    else()
      set(SECP256K1_RANLIB emranlib)
    endif()
    file(MAKE_DIRECTORY ${SECP256K1_BINARY_DIR})
    add_custom_command(
      WORKING_DIRECTORY ${SECP256K1_SOURCE_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${SECP256K1_EMSRC_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${SECP256K1_SOURCE_DIR} ${SECP256K1_EMSRC_DIR}
      COMMAND ${CMAKE_COMMAND} -E chdir ${SECP256K1_EMSRC_DIR} ./autogen.sh
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        AR=${SECP256K1_AR}
        RANLIB=${SECP256K1_RANLIB}
        ${CMAKE_COMMAND} -E chdir ${SECP256K1_EMSRC_DIR} emconfigure ./configure
        --enable-module-recovery
        --enable-module-extrakeys
        --disable-tests
        --disable-exhaustive-tests
        --disable-benchmark
        --disable-examples
        --prefix=${SECP256K1_BINARY_DIR}
        --with-pic
        --disable-shared
        --enable-static
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        AR=${SECP256K1_AR}
        RANLIB=${SECP256K1_RANLIB}
        ${CMAKE_COMMAND} -E chdir ${SECP256K1_EMSRC_DIR} emmake make clean
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        AR=${SECP256K1_AR}
        RANLIB=${SECP256K1_RANLIB}
        ${CMAKE_COMMAND} -E chdir ${SECP256K1_EMSRC_DIR} emmake make -j16
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        AR=${SECP256K1_AR}
        RANLIB=${SECP256K1_RANLIB}
        ${CMAKE_COMMAND} -E chdir ${SECP256K1_EMSRC_DIR} emmake make install
      COMMENT "Build Secp256k1 with emscripten"
      DEPENDS ${SECP256K1_SOURCE_DIR}/configure.ac
      OUTPUT ${SECP256K1_LIBRARY}
    )
  elseif (ANDROID OR NOT NIX)
    if (ANDROID)
      set(SECP256K1_BINARY_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/secp256k1/${TON_ANDROID_ARCH_DIR})
      set(SECP256K1_BUILD_DIR ${SECP256K1_BINARY_DIR}/src)
      set(SECP256K1_CC ${TON_ANDROID_CC})
      set(SECP256K1_CXX ${TON_ANDROID_CXX})
      set(SECP256K1_AR ${TON_ANDROID_AR})
      set(SECP256K1_RANLIB ${TON_ANDROID_RANLIB})
    elseif (MINGW)
      set(SECP256K1_CC ${CMAKE_C_COMPILER})
      set(SECP256K1_CXX ${CMAKE_CXX_COMPILER})
      set(SECP256K1_AR ar)
      set(SECP256K1_RANLIB ranlib)
    else()
      set(SECP256K1_CC ${CMAKE_C_COMPILER})
      set(SECP256K1_CXX ${CMAKE_CXX_COMPILER})
      set(SECP256K1_AR ${CMAKE_AR})
      if (CMAKE_RANLIB)
        set(SECP256K1_RANLIB ${CMAKE_RANLIB})
      else()
        set(SECP256K1_RANLIB ranlib)
      endif()
    endif()

    set(SECP256K1_LIBRARY ${SECP256K1_BINARY_DIR}/lib/libsecp256k1.a)
    set(SECP256K1_INCLUDE_DIR ${SECP256K1_BINARY_DIR}/include)
    set(SECP256K1_LIBRARY ${SECP256K1_LIBRARY} CACHE FILEPATH "Secp256k1 library" FORCE)
    set(SECP256K1_INCLUDE_DIR ${SECP256K1_INCLUDE_DIR} CACHE PATH "Secp256k1 include dir" FORCE)

    file(MAKE_DIRECTORY ${SECP256K1_BINARY_DIR}/lib)
    file(MAKE_DIRECTORY ${SECP256K1_INCLUDE_DIR})
    file(MAKE_DIRECTORY ${SECP256K1_BUILD_DIR})

    if (CMAKE_C_FLAGS)
      set(SECP256K1_CFLAGS "${CMAKE_C_FLAGS} -fPIC")
    else()
      set(SECP256K1_CFLAGS "-fPIC")
    endif()

    set(SECP256K1_CONFIGURE_ARGS
      -q
      --disable-option-checking
      --enable-module-recovery
      --enable-module-extrakeys
      --prefix=${SECP256K1_BINARY_DIR}
      --with-pic
      --disable-shared
      --enable-static
      --disable-tests
      --disable-exhaustive-tests
      --disable-benchmark
      --disable-examples
    )
    if (ANDROID)
      list(APPEND SECP256K1_CONFIGURE_ARGS --host=${TON_ANDROID_HOST})
    endif()

    if (MINGW)
      find_program(MSYS2_BASH bash)
      if (NOT MSYS2_BASH)
        message(FATAL_ERROR "bash not found in PATH; ensure MSYS2 is in PATH.")
      endif()
      string(REPLACE ";" " " SECP256K1_CONFIGURE_ARGS_STR "${SECP256K1_CONFIGURE_ARGS}")
      add_custom_command(
        WORKING_DIRECTORY ${SECP256K1_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${SECP256K1_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${SECP256K1_SOURCE_DIR} ${SECP256K1_BUILD_DIR}
        COMMAND ${MSYS2_BASH} -lc "cd '${SECP256K1_BUILD_DIR}' && ./autogen.sh"
        COMMAND ${MSYS2_BASH} -lc "cd '${SECP256K1_BUILD_DIR}' && CC='${SECP256K1_CC}' CXX='${SECP256K1_CXX}' AR='${SECP256K1_AR}' RANLIB='${SECP256K1_RANLIB}' CFLAGS='${SECP256K1_CFLAGS}' ./configure ${SECP256K1_CONFIGURE_ARGS_STR}"
        COMMAND ${MSYS2_BASH} -lc "cd '${SECP256K1_BUILD_DIR}' && CC='${SECP256K1_CC}' CXX='${SECP256K1_CXX}' AR='${SECP256K1_AR}' RANLIB='${SECP256K1_RANLIB}' CFLAGS='${SECP256K1_CFLAGS}' make clean"
        COMMAND ${MSYS2_BASH} -lc "cd '${SECP256K1_BUILD_DIR}' && CC='${SECP256K1_CC}' CXX='${SECP256K1_CXX}' AR='${SECP256K1_AR}' RANLIB='${SECP256K1_RANLIB}' CFLAGS='${SECP256K1_CFLAGS}' make -j16"
        COMMAND ${MSYS2_BASH} -lc "cd '${SECP256K1_BUILD_DIR}' && CC='${SECP256K1_CC}' CXX='${SECP256K1_CXX}' AR='${SECP256K1_AR}' RANLIB='${SECP256K1_RANLIB}' CFLAGS='${SECP256K1_CFLAGS}' make install"
        COMMAND ${CMAKE_COMMAND} -E echo "Skip ranlib on MinGW"
        COMMENT "Build secp256k1"
        DEPENDS ${SECP256K1_SOURCE_DIR}
        OUTPUT ${SECP256K1_LIBRARY}
      )
    else()
      add_custom_command(
        WORKING_DIRECTORY ${SECP256K1_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${SECP256K1_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${SECP256K1_SOURCE_DIR} ${SECP256K1_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -f ${SECP256K1_LIBRARY}
        COMMAND ${CMAKE_COMMAND} -E chdir ${SECP256K1_BUILD_DIR} ./autogen.sh
        COMMAND ${CMAKE_COMMAND} -E chdir ${SECP256K1_BUILD_DIR} ${CMAKE_COMMAND} -E env
          CC=${SECP256K1_CC}
          CXX=${SECP256K1_CXX}
          AR=${SECP256K1_AR}
          RANLIB=${SECP256K1_RANLIB}
          CFLAGS=${SECP256K1_CFLAGS}
          ./configure ${SECP256K1_CONFIGURE_ARGS}
        COMMAND ${CMAKE_COMMAND} -E chdir ${SECP256K1_BUILD_DIR} ${CMAKE_COMMAND} -E env
          CC=${SECP256K1_CC}
          CXX=${SECP256K1_CXX}
          AR=${SECP256K1_AR}
          RANLIB=${SECP256K1_RANLIB}
          CFLAGS=${SECP256K1_CFLAGS}
          make clean
        COMMAND ${CMAKE_COMMAND} -E chdir ${SECP256K1_BUILD_DIR} ${CMAKE_COMMAND} -E env
          CC=${SECP256K1_CC}
          CXX=${SECP256K1_CXX}
          AR=${SECP256K1_AR}
          RANLIB=${SECP256K1_RANLIB}
          CFLAGS=${SECP256K1_CFLAGS}
          make -j16
        COMMAND ${CMAKE_COMMAND} -E chdir ${SECP256K1_BUILD_DIR} ${CMAKE_COMMAND} -E env
          CC=${SECP256K1_CC}
          CXX=${SECP256K1_CXX}
          AR=${SECP256K1_AR}
          RANLIB=${SECP256K1_RANLIB}
          CFLAGS=${SECP256K1_CFLAGS}
          make install
        COMMAND ${SECP256K1_RANLIB} ${SECP256K1_LIBRARY}
        COMMENT "Build secp256k1"
        DEPENDS ${SECP256K1_SOURCE_DIR}
        OUTPUT ${SECP256K1_LIBRARY}
      )
    endif()
  endif()
else()
  message(STATUS "Use Secp256k1: ${SECP256K1_LIBRARY}")
endif()

add_custom_target(secp256k1 DEPENDS ${SECP256K1_LIBRARY})
