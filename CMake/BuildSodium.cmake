if (NOT SODIUM_LIBRARY)

    set(SODIUM_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sodium)
    set(SODIUM_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/sodium)
    set(SODIUM_INCLUDE_DIR ${SODIUM_BINARY_DIR}/include)

    file(MAKE_DIRECTORY ${SODIUM_BINARY_DIR})
    file(MAKE_DIRECTORY "${SODIUM_BINARY_DIR}/include")

    if (MSVC)
      set(SODIUM_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sodium)
      set(SODIUM_LIBRARY ${SODIUM_SOURCE_DIR}/build/src/Release/libsodium.lib)
      set(SODIUM_INCLUDE_DIR ${SODIUM_BINARY_DIR}/include)
      add_custom_command(
        WORKING_DIRECTORY ${SODIUM_SOURCE_DIR}
        COMMAND cmake -E env CFLAGS="/WX" cmake -A x64 -B build -DSODIUM_ENABLE_MODULE_RECOVERY=ON -DSODIUM_ENABLE_MODULE_EXTRAKEYS=ON -DSODIUM_BUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
        COMMAND cmake --build build --config Release
        COMMENT "Build Secp256k1"
        DEPENDS ${SODIUM_SOURCE_DIR}
        OUTPUT ${SODIUM_LIBRARY}
      )
    elseif (EMSCRIPTEN)
      set(SODIUM_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sodium)
      set(SODIUM_LIBRARY ${SODIUM_BINARY_DIR}/.libs/libsodium.a)
      set(SODIUM_INCLUDE_DIR ${SODIUM_SOURCE_DIR}/include)
      add_custom_command(
          WORKING_DIRECTORY ${SODIUM_SOURCE_DIR}
          COMMAND ./autogen.sh
          COMMAND emconfigure ./configure --enable-module-recovery --enable-module-extrakeys --disable-tests --disable-benchmark
          COMMAND emmake make clean
          COMMAND emmake make
          COMMENT "Build Secp256k1 with emscripten"
          DEPENDS ${SODIUM_SOURCE_DIR}
          OUTPUT ${SODIUM_LIBRARY}
      )
    else()
      if (NOT NIX)
        set(SODIUM_LIBRARY ${SODIUM_BINARY_DIR}/lib/libsodium.a)
        add_custom_command(
            WORKING_DIRECTORY ${SODIUM_SOURCE_DIR}
            COMMAND ./autogen.sh
            COMMAND ./configure --with-pic --enable-static
            COMMAND make -j16
            COMMAND make install
            COMMENT "Build sodium"
            DEPENDS ${SODIUM_SOURCE_DIR}
            OUTPUT ${SODIUM_LIBRARY}
        )
      endif()
    endif()
else()
   message(STATUS "Use Secp256k1: ${SODIUM_LIBRARY}")
endif()

add_custom_target(sodium DEPENDS ${SODIUM_LIBRARY})
