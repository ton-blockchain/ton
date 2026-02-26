include(AndroidThirdParty)

get_filename_component(TON_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TON_THIRD_PARTY_SOURCE_DIR "${TON_SOURCE_DIR}/third-party")
set(TON_THIRD_PARTY_BINARY_DIR "${CMAKE_BINARY_DIR}/third-party")

set(SODIUM_SOURCE_DIR ${TON_THIRD_PARTY_SOURCE_DIR}/sodium)
set(SODIUM_BINARY_DIR ${TON_THIRD_PARTY_BINARY_DIR}/sodium)
set(SODIUM_BUILD_DIR ${SODIUM_BINARY_DIR}/src)

if (USE_EMSCRIPTEN OR EMSCRIPTEN)
  set(SODIUM_ROOT_DIR ${CMAKE_BINARY_DIR}/3pp_emscripten/libsodium)
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
  set(SODIUM_LIBRARY ${SODIUM_BINARY_DIR}/lib/libsodium.lib CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_RELEASE ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_DEBUG ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium debug library" FORCE)
  set(SODIUM_INCLUDE_DIR ${SODIUM_SOURCE_DIR}/src/libsodium/include CACHE PATH "Sodium include dir" FORCE)
  set(SODIUM_FOUND TRUE CACHE BOOL "Sodium found" FORCE)
  file(MAKE_DIRECTORY ${SODIUM_BINARY_DIR}/lib)
  file(MAKE_DIRECTORY ${SODIUM_BINARY_DIR}/obj)
  file(TO_NATIVE_PATH "${SODIUM_BINARY_DIR}/lib/" SODIUM_MSVC_OUT_DIR)
  file(TO_NATIVE_PATH "${SODIUM_BINARY_DIR}/obj/" SODIUM_MSVC_INT_DIR)
  if (NOT EXISTS "${SODIUM_LIBRARY}")
    execute_process(
      COMMAND msbuild libsodium.vcxproj /p:Configuration=ReleaseLIB /p:Platform=x64 -p:PlatformToolset=v143 /p:OutDir=${SODIUM_MSVC_OUT_DIR} /p:IntDir=${SODIUM_MSVC_INT_DIR}
      WORKING_DIRECTORY ${SODIUM_PROJECT_DIR}
      RESULT_VARIABLE SODIUM_BUILD_RESULT
    )
    if (NOT SODIUM_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "Sodium build failed with code ${SODIUM_BUILD_RESULT}")
    endif()
  endif()
  add_custom_command(
    WORKING_DIRECTORY ${SODIUM_PROJECT_DIR}
    COMMAND msbuild libsodium.vcxproj /p:Configuration=ReleaseLIB /p:Platform=x64 -p:PlatformToolset=v143 /p:OutDir=${SODIUM_MSVC_OUT_DIR} /p:IntDir=${SODIUM_MSVC_INT_DIR}
    COMMENT "Build sodium (MSVC)"
    DEPENDS ${SODIUM_SOURCE_DIR}
    OUTPUT ${SODIUM_LIBRARY}
  )
elseif (ANDROID OR NOT NIX)
  if (ANDROID)
    set(SODIUM_BINARY_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/libsodium/${TON_ANDROID_SODIUM_DIR})
    set(SODIUM_BUILD_DIR ${SODIUM_BINARY_DIR}/src)
    set(SODIUM_CC ${TON_ANDROID_CC})
    set(SODIUM_CXX ${TON_ANDROID_CXX})
    set(SODIUM_AR ${TON_ANDROID_AR})
    set(SODIUM_RANLIB ${TON_ANDROID_RANLIB})
  elseif (MINGW)
    set(SODIUM_CC ${CMAKE_C_COMPILER})
    set(SODIUM_CXX ${CMAKE_CXX_COMPILER})
    set(SODIUM_AR ar)
    set(SODIUM_RANLIB ranlib)
  else()
    set(SODIUM_CC ${CMAKE_C_COMPILER})
    set(SODIUM_CXX ${CMAKE_CXX_COMPILER})
    set(SODIUM_AR ${CMAKE_AR})
    if (CMAKE_RANLIB)
      set(SODIUM_RANLIB ${CMAKE_RANLIB})
    else()
      set(SODIUM_RANLIB ranlib)
    endif()
  endif()

  set(SODIUM_INCLUDE_DIR ${SODIUM_BINARY_DIR}/include)
  set(SODIUM_LIBRARY ${SODIUM_BINARY_DIR}/lib/libsodium.a CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_RELEASE ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium release library" FORCE)
  set(SODIUM_LIBRARY_DEBUG ${SODIUM_LIBRARY} CACHE FILEPATH "Sodium debug library" FORCE)
  set(SODIUM_INCLUDE_DIR ${SODIUM_INCLUDE_DIR} CACHE PATH "Sodium include dir" FORCE)
  set(SODIUM_FOUND TRUE CACHE BOOL "Sodium found" FORCE)

  file(MAKE_DIRECTORY ${SODIUM_BINARY_DIR}/lib)
  file(MAKE_DIRECTORY ${SODIUM_INCLUDE_DIR})
  file(MAKE_DIRECTORY ${SODIUM_BUILD_DIR})

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

  set(SODIUM_CONFIGURE_ARGS
    --prefix=${SODIUM_BINARY_DIR}
    --with-pic
    --enable-static
    --disable-shared
  )
  if (ANDROID)
    list(APPEND SODIUM_CONFIGURE_ARGS --host=${TON_ANDROID_HOST})
  endif()

  if (MINGW)
    find_program(MSYS2_BASH bash)
    if (NOT MSYS2_BASH)
      message(FATAL_ERROR "bash not found in PATH; ensure MSYS2 is in PATH.")
    endif()
    string(REPLACE ";" " " SODIUM_CONFIGURE_ARGS_STR "${SODIUM_CONFIGURE_ARGS}")
    add_custom_command(
      WORKING_DIRECTORY ${SODIUM_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${SODIUM_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${SODIUM_SOURCE_DIR} ${SODIUM_BUILD_DIR}
      COMMAND ${MSYS2_BASH} -lc "cd '${SODIUM_BUILD_DIR}' && CC='${SODIUM_CC}' CXX='${SODIUM_CXX}' AR='${SODIUM_AR}' RANLIB='${SODIUM_RANLIB}' CFLAGS='${SODIUM_CFLAGS}' CXXFLAGS='${SODIUM_CXXFLAGS}' ./configure ${SODIUM_CONFIGURE_ARGS_STR}"
      COMMAND ${MSYS2_BASH} -lc "cd '${SODIUM_BUILD_DIR}' && CC='${SODIUM_CC}' CXX='${SODIUM_CXX}' AR='${SODIUM_AR}' RANLIB='${SODIUM_RANLIB}' CFLAGS='${SODIUM_CFLAGS}' CXXFLAGS='${SODIUM_CXXFLAGS}' make clean"
      COMMAND ${MSYS2_BASH} -lc "cd '${SODIUM_BUILD_DIR}' && CC='${SODIUM_CC}' CXX='${SODIUM_CXX}' AR='${SODIUM_AR}' RANLIB='${SODIUM_RANLIB}' CFLAGS='${SODIUM_CFLAGS}' CXXFLAGS='${SODIUM_CXXFLAGS}' make -j16"
      COMMAND ${MSYS2_BASH} -lc "cd '${SODIUM_BUILD_DIR}' && CC='${SODIUM_CC}' CXX='${SODIUM_CXX}' AR='${SODIUM_AR}' RANLIB='${SODIUM_RANLIB}' CFLAGS='${SODIUM_CFLAGS}' CXXFLAGS='${SODIUM_CXXFLAGS}' make install"
      COMMAND ${CMAKE_COMMAND} -E echo "Skip ranlib on MinGW"
      COMMENT "Build sodium"
      DEPENDS ${SODIUM_SOURCE_DIR}
      OUTPUT ${SODIUM_LIBRARY}
    )
  else()
    add_custom_command(
      WORKING_DIRECTORY ${SODIUM_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${SODIUM_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${SODIUM_SOURCE_DIR} ${SODIUM_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -f ${SODIUM_LIBRARY}
      COMMAND ${CMAKE_COMMAND} -E chdir ${SODIUM_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${SODIUM_CC}
        CXX=${SODIUM_CXX}
        AR=${SODIUM_AR}
        RANLIB=${SODIUM_RANLIB}
        CFLAGS=${SODIUM_CFLAGS}
        CXXFLAGS=${SODIUM_CXXFLAGS}
        ./configure ${SODIUM_CONFIGURE_ARGS}
      COMMAND ${CMAKE_COMMAND} -E chdir ${SODIUM_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${SODIUM_CC}
        CXX=${SODIUM_CXX}
        AR=${SODIUM_AR}
        RANLIB=${SODIUM_RANLIB}
        CFLAGS=${SODIUM_CFLAGS}
        CXXFLAGS=${SODIUM_CXXFLAGS}
        make clean
      COMMAND ${CMAKE_COMMAND} -E chdir ${SODIUM_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${SODIUM_CC}
        CXX=${SODIUM_CXX}
        AR=${SODIUM_AR}
        RANLIB=${SODIUM_RANLIB}
        CFLAGS=${SODIUM_CFLAGS}
        CXXFLAGS=${SODIUM_CXXFLAGS}
        make -j16
      COMMAND ${CMAKE_COMMAND} -E chdir ${SODIUM_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${SODIUM_CC}
        CXX=${SODIUM_CXX}
        AR=${SODIUM_AR}
        RANLIB=${SODIUM_RANLIB}
        CFLAGS=${SODIUM_CFLAGS}
        CXXFLAGS=${SODIUM_CXXFLAGS}
        make install
      COMMAND ${SODIUM_RANLIB} ${SODIUM_LIBRARY}
      COMMENT "Build sodium"
      DEPENDS ${SODIUM_SOURCE_DIR}
      OUTPUT ${SODIUM_LIBRARY}
    )
  endif()
else()
  message(STATUS "Use sodium: ${SODIUM_LIBRARY}")
endif()

add_custom_target(sodium DEPENDS ${SODIUM_LIBRARY})
