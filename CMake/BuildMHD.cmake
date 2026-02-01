include(AndroidThirdParty)

get_filename_component(TON_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TON_THIRD_PARTY_SOURCE_DIR "${TON_SOURCE_DIR}/third-party")
set(TON_THIRD_PARTY_BINARY_DIR "${CMAKE_BINARY_DIR}/third-party")

set(MHD_SOURCE_DIR ${TON_THIRD_PARTY_SOURCE_DIR}/libmicrohttpd)
set(MHD_BINARY_DIR ${TON_THIRD_PARTY_BINARY_DIR}/libmicrohttpd)

if (USE_EMSCRIPTEN OR EMSCRIPTEN)
  message(STATUS "libmicrohttpd is not built for emscripten")
  set(MHD_FOUND FALSE CACHE BOOL "MHD found" FORCE)
elseif (MSVC)
  set(MHD_PROJECT_DIR ${MHD_SOURCE_DIR}/w32/VS2022)
  set(MHD_LIBRARY ${MHD_PROJECT_DIR}/Output/x64/libmicrohttpd.lib)
  set(MHD_INCLUDE_DIR ${MHD_SOURCE_DIR}/src/include)

  if (NOT EXISTS "${MHD_LIBRARY}")
    execute_process(
      COMMAND msbuild libmicrohttpd.vcxproj /p:Configuration=Release-static /p:platform=x64 -p:PlatformToolset=v143
      WORKING_DIRECTORY ${MHD_PROJECT_DIR}
      RESULT_VARIABLE MHD_BUILD_RESULT
    )
    if (NOT MHD_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "libmicrohttpd build failed with code ${MHD_BUILD_RESULT}")
    endif()
  endif()
  add_custom_command(
    WORKING_DIRECTORY ${MHD_PROJECT_DIR}
    COMMAND msbuild libmicrohttpd.vcxproj /p:Configuration=Release-static /p:platform=x64 -p:PlatformToolset=v143
    COMMENT "Build libmicrohttpd (MSVC)"
    DEPENDS ${MHD_SOURCE_DIR}
    OUTPUT ${MHD_LIBRARY}
  )
elseif (ANDROID)
  set(MHD_BINARY_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/libmicrohttpd/${TON_ANDROID_ARCH_DIR})
  set(MHD_LIBRARY ${MHD_BINARY_DIR}/lib/libmicrohttpd.a)
  set(MHD_INCLUDE_DIR ${MHD_BINARY_DIR}/include)

  file(MAKE_DIRECTORY ${MHD_BINARY_DIR})
  file(MAKE_DIRECTORY ${MHD_INCLUDE_DIR})

  if (CMAKE_C_FLAGS)
    set(MHD_CFLAGS "${CMAKE_C_FLAGS} -fPIC")
  else()
    set(MHD_CFLAGS "-fPIC")
  endif()

  add_custom_command(
      WORKING_DIRECTORY ${MHD_SOURCE_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -f ${MHD_LIBRARY}
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${TON_ANDROID_AR}
        RANLIB=${TON_ANDROID_RANLIB}
        CFLAGS=${MHD_CFLAGS}
        ./configure --host=${TON_ANDROID_HOST} --prefix=${MHD_BINARY_DIR} --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic --disable-doc
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${TON_ANDROID_AR}
        RANLIB=${TON_ANDROID_RANLIB}
        CFLAGS=${MHD_CFLAGS}
        make clean
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${TON_ANDROID_AR}
        RANLIB=${TON_ANDROID_RANLIB}
        CFLAGS=${MHD_CFLAGS}
        make -j16
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${TON_ANDROID_CC}
        CXX=${TON_ANDROID_CXX}
        AR=${TON_ANDROID_AR}
        RANLIB=${TON_ANDROID_RANLIB}
        CFLAGS=${MHD_CFLAGS}
        make install
      COMMAND ${TON_ANDROID_RANLIB} ${MHD_LIBRARY}
      COMMENT "Build libmicrohttpd (Android)"
      DEPENDS ${MHD_SOURCE_DIR}
      OUTPUT ${MHD_LIBRARY}
  )
else()
  set(MHD_INCLUDE_DIR ${MHD_SOURCE_DIR}/src/include)
  set(MHD_LIBRARY ${MHD_SOURCE_DIR}/src/microhttpd/.libs/libmicrohttpd.a)

  if (MINGW)
    find_program(MSYS2_BASH bash)
    if (NOT MSYS2_BASH)
      message(FATAL_ERROR "bash not found in PATH; ensure MSYS2 is in PATH.")
    endif()
    add_custom_command(
        WORKING_DIRECTORY ${MHD_SOURCE_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -f ${MHD_LIBRARY}
        COMMAND ${CMAKE_COMMAND} -E touch ${MHD_SOURCE_DIR}/aclocal.m4
        COMMAND ${MSYS2_BASH} -lc "./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic --disable-doc"
        COMMAND ${MSYS2_BASH} -lc "make clean ACLOCAL=: AUTOMAKE=: AUTOCONF=: AUTOHEADER=:"
        COMMAND ${MSYS2_BASH} -lc "make -j16 ACLOCAL=: AUTOMAKE=: AUTOCONF=: AUTOHEADER=:"
        COMMENT "Build libmicrohttpd"
        DEPENDS ${MHD_SOURCE_DIR}
        OUTPUT ${MHD_LIBRARY}
    )
  else()
    add_custom_command(
        WORKING_DIRECTORY ${MHD_SOURCE_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -f ${MHD_LIBRARY}
        COMMAND ${CMAKE_COMMAND} -E env
          CC=${CMAKE_C_COMPILER}
          CXX=${CMAKE_CXX_COMPILER}
          ./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic --disable-doc
        COMMAND ${CMAKE_COMMAND} -E env
          CC=${CMAKE_C_COMPILER}
          CXX=${CMAKE_CXX_COMPILER}
          make clean
        COMMAND ${CMAKE_COMMAND} -E env
          CC=${CMAKE_C_COMPILER}
          CXX=${CMAKE_CXX_COMPILER}
          make -j16
        COMMENT "Build libmicrohttpd"
        DEPENDS ${MHD_SOURCE_DIR}
        OUTPUT ${MHD_LIBRARY}
    )
  endif()
endif()

if (MHD_LIBRARY)
  set(MHD_LIBRARY ${MHD_LIBRARY} CACHE FILEPATH "MHD library" FORCE)
  set(MHD_INCLUDE_DIR ${MHD_INCLUDE_DIR} CACHE PATH "MHD include dir" FORCE)
  set(MHD_FOUND TRUE CACHE BOOL "MHD found" FORCE)
  add_custom_target(mhd DEPENDS ${MHD_LIBRARY})
endif()
