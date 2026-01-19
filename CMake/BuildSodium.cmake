set(SODIUM_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sodium)
set(SODIUM_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/sodium)

if (USE_EMSCRIPTEN OR EMSCRIPTEN)
  set(SODIUM_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3pp_emscripten/libsodium)
  if (EXISTS ${SODIUM_ROOT_DIR}/lib/libsodium.a)
    set(SODIUM_LIBRARY ${SODIUM_ROOT_DIR}/lib/libsodium.a)
    set(SODIUM_INCLUDE_DIR ${SODIUM_ROOT_DIR}/include)
  elseif (EXISTS ${SODIUM_ROOT_DIR}/src/libsodium/.libs/libsodium.a)
    set(SODIUM_LIBRARY ${SODIUM_ROOT_DIR}/src/libsodium/.libs/libsodium.a)
    set(SODIUM_INCLUDE_DIR ${SODIUM_ROOT_DIR}/src/libsodium/include)
  else()
    message(FATAL_ERROR "libsodium not found under ${SODIUM_ROOT_DIR}. Build it with emscripten first.")
  endif()
  set(SODIUM_LIBRARY_RELEASE ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_DEBUG ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium debug library" FORCE)
  set(SODIUM_INCLUDE_DIR ${SODIUM_INCLUDE_DIR} CACHE PATH "Sodium include dir" FORCE)
  set(SODIUM_FOUND TRUE CACHE BOOL "Sodium found" FORCE)
elseif (MSVC)
  set(SODIUM_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sodium)
  set(SODIUM_LIBRARY ${SODIUM_SOURCE_DIR}/build/src/Release/libsodium.lib CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_RELEASE ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_DEBUG ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium debug library" FORCE)
  set(SODIUM_INCLUDE_DIR ${SODIUM_BINARY_DIR}/include CACHE PATH "Sodium include dir" FORCE)
  set(SODIUM_FOUND TRUE CACHE BOOL "Sodium found" FORCE)
  add_custom_command(
    WORKING_DIRECTORY ${SODIUM_SOURCE_DIR}
    COMMAND cmake -E env CFLAGS="/WX" cmake -A x64 -B build -DSODIUM_ENABLE_MODULE_RECOVERY=ON -DSODIUM_ENABLE_MODULE_EXTRAKEYS=ON -DSODIUM_BUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
    COMMAND cmake --build build --config Release
    COMMENT "Build Secp256k1"
    DEPENDS ${SODIUM_SOURCE_DIR}
    OUTPUT ${SODIUM_LIBRARY}
  )
elseif (NOT NIX)
  set(SODIUM_INCLUDE_DIR ${SODIUM_BINARY_DIR}/include)
  set(SODIUM_LIBRARY ${SODIUM_BINARY_DIR}/lib/libsodium.a CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_RELEASE ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_DEBUG ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium debug library" FORCE)
  set(SODIUM_INCLUDE_DIR ${SODIUM_INCLUDE_DIR} CACHE PATH "Sodium include dir" FORCE)
  set(SODIUM_FOUND TRUE CACHE BOOL "Sodium found" FORCE)

  file(MAKE_DIRECTORY ${SODIUM_BINARY_DIR})
  file(MAKE_DIRECTORY "${SODIUM_BINARY_DIR}/include")

  if (CMAKE_RANLIB)
    set(SODIUM_RANLIB ${CMAKE_RANLIB})
  else()
    set(SODIUM_RANLIB ranlib)
  endif()
  add_custom_command(
      WORKING_DIRECTORY ${SODIUM_SOURCE_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -f ${SODIUM_LIBRARY}
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        AR=${CMAKE_AR}
        RANLIB=${SODIUM_RANLIB}
        ./autogen.sh
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        AR=${CMAKE_AR}
        RANLIB=${SODIUM_RANLIB}
        ./configure --prefix=${SODIUM_BINARY_DIR} --with-pic --enable-static --disable-shared
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        AR=${CMAKE_AR}
        RANLIB=${SODIUM_RANLIB}
        make clean
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        AR=${CMAKE_AR}
        RANLIB=${SODIUM_RANLIB}
        make -j16
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        AR=${CMAKE_AR}
        RANLIB=${SODIUM_RANLIB}
        make install
      COMMAND ${SODIUM_RANLIB} ${SODIUM_LIBRARY}
      COMMENT "Build sodium"
      DEPENDS ${SODIUM_SOURCE_DIR}
      OUTPUT ${SODIUM_LIBRARY}
  )
else()
  message(STATUS "Use Secp256k1: ${SODIUM_LIBRARY}")
endif()

add_custom_target(sodium DEPENDS ${SODIUM_LIBRARY})
