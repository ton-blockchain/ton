include(AndroidThirdParty)

get_filename_component(TON_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TON_THIRD_PARTY_SOURCE_DIR "${TON_SOURCE_DIR}/third-party")
set(TON_THIRD_PARTY_BINARY_DIR "${CMAKE_BINARY_DIR}/third-party")

set(SODIUM_SOURCE_DIR ${TON_THIRD_PARTY_SOURCE_DIR}/sodium)
set(SODIUM_BINARY_DIR ${TON_THIRD_PARTY_BINARY_DIR}/sodium)

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
  set(SODIUM_PROJECT_DIR ${SODIUM_SOURCE_DIR}/builds/msvc/vs2022/libsodium)
  set(SODIUM_MSVC_OUT_DIR ${SODIUM_PROJECT_DIR}/x64/Release)
  set(SODIUM_LIBRARY ${SODIUM_MSVC_OUT_DIR}/libsodium.lib CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_RELEASE ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_DEBUG ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium debug library" FORCE)
  set(SODIUM_INCLUDE_DIR ${SODIUM_SOURCE_DIR}/src/libsodium/include CACHE PATH "Sodium include dir" FORCE)
  set(SODIUM_FOUND TRUE CACHE BOOL "Sodium found" FORCE)
  if (NOT EXISTS "${SODIUM_LIBRARY}")
    execute_process(
      COMMAND msbuild libsodium.vcxproj /p:Configuration=ReleaseLIB /p:Platform=x64 -p:PlatformToolset=v143 /p:OutDir=${SODIUM_MSVC_OUT_DIR}\\ /p:IntDir=${SODIUM_MSVC_OUT_DIR}\\obj\\
      WORKING_DIRECTORY ${SODIUM_PROJECT_DIR}
      RESULT_VARIABLE SODIUM_BUILD_RESULT
    )
    if (NOT SODIUM_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "Sodium build failed with code ${SODIUM_BUILD_RESULT}")
    endif()
  endif()
  add_custom_command(
    WORKING_DIRECTORY ${SODIUM_PROJECT_DIR}
    COMMAND msbuild libsodium.vcxproj /p:Configuration=ReleaseLIB /p:Platform=x64 -p:PlatformToolset=v143 /p:OutDir=${SODIUM_MSVC_OUT_DIR}\\ /p:IntDir=${SODIUM_MSVC_OUT_DIR}\\obj\\
    COMMENT "Build sodium (MSVC)"
    DEPENDS ${SODIUM_SOURCE_DIR}
    OUTPUT ${SODIUM_LIBRARY}
  )
elseif (ANDROID)
  set(SODIUM_BINARY_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/libsodium/${TON_ANDROID_SODIUM_DIR})
  set(SODIUM_INCLUDE_DIR ${SODIUM_BINARY_DIR}/include)
  set(SODIUM_LIBRARY ${SODIUM_BINARY_DIR}/lib/libsodium.a CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_RELEASE ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_DEBUG ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium debug library" FORCE)
  set(SODIUM_INCLUDE_DIR ${SODIUM_INCLUDE_DIR} CACHE PATH "Sodium include dir" FORCE)
  set(SODIUM_FOUND TRUE CACHE BOOL "Sodium found" FORCE)

  file(MAKE_DIRECTORY ${SODIUM_BINARY_DIR})
  file(MAKE_DIRECTORY "${SODIUM_BINARY_DIR}/include")

  set(SODIUM_AR ${TON_ANDROID_AR})
  set(SODIUM_RANLIB ${TON_ANDROID_RANLIB})
  if (CMAKE_C_FLAGS)
    set(SODIUM_CFLAGS "${CMAKE_C_FLAGS} -fPIC")
  else()
    set(SODIUM_CFLAGS "-fPIC")
  endif()
  if (CMAKE_CXX_FLAGS)
    set(SODIUM_CXXFLAGS "${CMAKE_CXX_FLAGS} -fPIC")
  else()
    set(SODIUM_CXXFLAGS "-fPIC")
  endif()
  add_custom_command(
      WORKING_DIRECTORY ${SODIUM_SOURCE_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -f ${SODIUM_LIBRARY}
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${SODIUM_AR}
        RANLIB=${SODIUM_RANLIB}
        CFLAGS=${SODIUM_CFLAGS}
        CXXFLAGS=${SODIUM_CXXFLAGS}
        ./autogen.sh
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${SODIUM_AR}
        RANLIB=${SODIUM_RANLIB}
        CFLAGS=${SODIUM_CFLAGS}
        CXXFLAGS=${SODIUM_CXXFLAGS}
        ./configure --host=${TON_ANDROID_HOST} --prefix=${SODIUM_BINARY_DIR} --with-pic --enable-static --disable-shared
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${SODIUM_AR}
        RANLIB=${SODIUM_RANLIB}
        CFLAGS=${SODIUM_CFLAGS}
        CXXFLAGS=${SODIUM_CXXFLAGS}
        make clean
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${SODIUM_AR}
        RANLIB=${SODIUM_RANLIB}
        CFLAGS=${SODIUM_CFLAGS}
        CXXFLAGS=${SODIUM_CXXFLAGS}
        make -j16
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${SODIUM_AR}
        RANLIB=${SODIUM_RANLIB}
        CFLAGS=${SODIUM_CFLAGS}
        CXXFLAGS=${SODIUM_CXXFLAGS}
        make install
      COMMAND ${SODIUM_RANLIB} ${SODIUM_LIBRARY}
      COMMENT "Build sodium (Android)"
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
