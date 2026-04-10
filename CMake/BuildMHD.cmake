include(AndroidThirdParty)

get_filename_component(TON_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TON_THIRD_PARTY_SOURCE_DIR "${TON_SOURCE_DIR}/third-party")
set(TON_THIRD_PARTY_BINARY_DIR "${CMAKE_BINARY_DIR}/third-party")

set(MHD_SOURCE_DIR ${TON_THIRD_PARTY_SOURCE_DIR}/libmicrohttpd)
set(MHD_BINARY_DIR ${TON_THIRD_PARTY_BINARY_DIR}/libmicrohttpd)
set(MHD_BUILD_DIR ${MHD_BINARY_DIR}/src)

if (USE_EMSCRIPTEN OR EMSCRIPTEN)
  message(STATUS "libmicrohttpd is not built for emscripten")
  set(MHD_FOUND FALSE CACHE BOOL "MHD found" FORCE)
elseif (MSVC)
  set(MHD_MSVC_TOOLSET "${CMAKE_VS_PLATFORM_TOOLSET}")
  if (MHD_MSVC_TOOLSET)
    string(REGEX REPLACE ",.*$" "" MHD_MSVC_TOOLSET "${MHD_MSVC_TOOLSET}")
  elseif (MSVC_VERSION GREATER_EQUAL 1930)
    set(MHD_MSVC_TOOLSET v143)
  elseif (MSVC_VERSION GREATER_EQUAL 1920)
    set(MHD_MSVC_TOOLSET v142)
  endif()

  if (MHD_MSVC_TOOLSET STREQUAL "v142")
    set(MHD_MSVC_PROJECT_SUBDIR VS2019)
  elseif (MHD_MSVC_TOOLSET STREQUAL "v143")
    set(MHD_MSVC_PROJECT_SUBDIR VS2022)
  elseif (MSVC_VERSION GREATER_EQUAL 1930)
    set(MHD_MSVC_PROJECT_SUBDIR VS2022)
  else()
    set(MHD_MSVC_PROJECT_SUBDIR VS2019)
  endif()

  set(MHD_PROJECT_DIR ${MHD_SOURCE_DIR}/w32/${MHD_MSVC_PROJECT_SUBDIR})
  set(MHD_LIBRARY ${MHD_BINARY_DIR}/lib/libmicrohttpd.lib)
  set(MHD_INCLUDE_DIR ${MHD_SOURCE_DIR}/src/include)
  file(MAKE_DIRECTORY ${MHD_BINARY_DIR}/lib)
  file(MAKE_DIRECTORY ${MHD_BINARY_DIR}/obj)
  set(MHD_MSVC_OUT_DIR "${MHD_BINARY_DIR}/lib/")
  set(MHD_MSVC_INT_DIR "${MHD_BINARY_DIR}/obj/")

  set(MHD_MSBUILD_ARGS
    libmicrohttpd.vcxproj
    /p:Configuration=Release-static
    /p:platform=x64
    /p:OutDir=${MHD_MSVC_OUT_DIR}
    /p:IntDir=${MHD_MSVC_INT_DIR}
  )
  if (MHD_MSVC_TOOLSET)
    list(APPEND MHD_MSBUILD_ARGS /p:PlatformToolset=${MHD_MSVC_TOOLSET})
  endif()

  if (NOT EXISTS "${MHD_LIBRARY}")
    execute_process(
      COMMAND msbuild ${MHD_MSBUILD_ARGS}
      WORKING_DIRECTORY ${MHD_PROJECT_DIR}
      RESULT_VARIABLE MHD_BUILD_RESULT
    )
    if (NOT MHD_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "libmicrohttpd build failed with code ${MHD_BUILD_RESULT}")
    endif()
  endif()
  add_custom_command(
    WORKING_DIRECTORY ${MHD_PROJECT_DIR}
    COMMAND msbuild ${MHD_MSBUILD_ARGS}
    COMMENT "Build libmicrohttpd (MSVC)"
    DEPENDS ${MHD_SOURCE_DIR}
    OUTPUT ${MHD_LIBRARY}
  )
else()
  if (ANDROID)
    set(MHD_BINARY_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/libmicrohttpd/${TON_ANDROID_ARCH_DIR})
    set(MHD_BUILD_DIR ${MHD_BINARY_DIR}/src)
  endif()
  set(MHD_LIBRARY ${MHD_BINARY_DIR}/lib/libmicrohttpd.a)
  set(MHD_INCLUDE_DIR ${MHD_BINARY_DIR}/include)
  file(MAKE_DIRECTORY ${MHD_BINARY_DIR}/lib)
  file(MAKE_DIRECTORY ${MHD_INCLUDE_DIR})
  file(MAKE_DIRECTORY ${MHD_BUILD_DIR})

  if (CMAKE_C_FLAGS)
    set(MHD_CFLAGS "${CMAKE_C_FLAGS} -fPIC")
  else()
    set(MHD_CFLAGS "-fPIC")
  endif()

  set(MHD_CONFIGURE_ARGS
    --prefix=${MHD_BINARY_DIR}
    --enable-static
    --disable-tests
    --disable-benchmark
    --disable-shared
    --disable-https
    --with-pic
    --disable-doc
  )
  if (ANDROID)
    list(APPEND MHD_CONFIGURE_ARGS --host=${TON_ANDROID_HOST})
  endif()

  if (MINGW)
    find_program(MSYS2_BASH bash)
    if (NOT MSYS2_BASH)
      message(FATAL_ERROR "bash not found in PATH; ensure MSYS2 is in PATH.")
    endif()
    string(REPLACE ";" " " MHD_CONFIGURE_ARGS_STR "${MHD_CONFIGURE_ARGS}")
    add_custom_command(
      WORKING_DIRECTORY ${MHD_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${MHD_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${MHD_SOURCE_DIR} ${MHD_BUILD_DIR}
      COMMAND ${MSYS2_BASH} -lc "cd '${MHD_BUILD_DIR}' && CC='${CMAKE_C_COMPILER}' CXX='${CMAKE_CXX_COMPILER}' AR='${CMAKE_AR}' RANLIB='${CMAKE_RANLIB}' CFLAGS='${MHD_CFLAGS}' ./configure ${MHD_CONFIGURE_ARGS_STR}"
      COMMAND ${MSYS2_BASH} -lc "cd '${MHD_BUILD_DIR}' && CC='${CMAKE_C_COMPILER}' CXX='${CMAKE_CXX_COMPILER}' AR='${CMAKE_AR}' RANLIB='${CMAKE_RANLIB}' CFLAGS='${MHD_CFLAGS}' make clean ACLOCAL=: AUTOMAKE=: AUTOCONF=: AUTOHEADER=:"
      COMMAND ${MSYS2_BASH} -lc "cd '${MHD_BUILD_DIR}' && CC='${CMAKE_C_COMPILER}' CXX='${CMAKE_CXX_COMPILER}' AR='${CMAKE_AR}' RANLIB='${CMAKE_RANLIB}' CFLAGS='${MHD_CFLAGS}' make -j16 ACLOCAL=: AUTOMAKE=: AUTOCONF=: AUTOHEADER=:"
      COMMAND ${MSYS2_BASH} -lc "cd '${MHD_BUILD_DIR}' && CC='${CMAKE_C_COMPILER}' CXX='${CMAKE_CXX_COMPILER}' AR='${CMAKE_AR}' RANLIB='${CMAKE_RANLIB}' CFLAGS='${MHD_CFLAGS}' make install ACLOCAL=: AUTOMAKE=: AUTOCONF=: AUTOHEADER=:"
      COMMENT "Build libmicrohttpd"
      DEPENDS ${MHD_SOURCE_DIR}
      OUTPUT ${MHD_LIBRARY}
    )
  else()
    if (ANDROID)
      set(MHD_CC ${TON_ANDROID_CC})
      set(MHD_CXX ${TON_ANDROID_CXX})
      set(MHD_AR ${TON_ANDROID_AR})
      set(MHD_RANLIB ${TON_ANDROID_RANLIB})
    else()
      set(MHD_CC ${CMAKE_C_COMPILER})
      set(MHD_CXX ${CMAKE_CXX_COMPILER})
      set(MHD_AR ${CMAKE_AR})
      set(MHD_RANLIB ${CMAKE_RANLIB})
    endif()
    if (NOT MHD_RANLIB)
      find_program(MHD_RANLIB ranlib)
    endif()
    set(MHD_POST_INSTALL_COMMANDS)
    if (MHD_RANLIB)
      list(APPEND MHD_POST_INSTALL_COMMANDS COMMAND ${MHD_RANLIB} ${MHD_LIBRARY})
    endif()

    add_custom_command(
      WORKING_DIRECTORY ${MHD_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${MHD_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${MHD_SOURCE_DIR} ${MHD_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -f ${MHD_LIBRARY}
      COMMAND ${CMAKE_COMMAND} -E chdir ${MHD_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${MHD_CC}
        CXX=${MHD_CXX}
        AR=${MHD_AR}
        RANLIB=${MHD_RANLIB}
        CFLAGS=${MHD_CFLAGS}
        ./configure ${MHD_CONFIGURE_ARGS}
      COMMAND ${CMAKE_COMMAND} -E chdir ${MHD_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${MHD_CC}
        CXX=${MHD_CXX}
        AR=${MHD_AR}
        RANLIB=${MHD_RANLIB}
        CFLAGS=${MHD_CFLAGS}
        ACLOCAL=:
        AUTOMAKE=:
        AUTOCONF=:
        AUTOHEADER=:
        make clean
      COMMAND ${CMAKE_COMMAND} -E chdir ${MHD_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${MHD_CC}
        CXX=${MHD_CXX}
        AR=${MHD_AR}
        RANLIB=${MHD_RANLIB}
        CFLAGS=${MHD_CFLAGS}
        ACLOCAL=:
        AUTOMAKE=:
        AUTOCONF=:
        AUTOHEADER=:
        make -j16
      COMMAND ${CMAKE_COMMAND} -E chdir ${MHD_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${MHD_CC}
        CXX=${MHD_CXX}
        AR=${MHD_AR}
        RANLIB=${MHD_RANLIB}
        CFLAGS=${MHD_CFLAGS}
        ACLOCAL=:
        AUTOMAKE=:
        AUTOCONF=:
        AUTOHEADER=:
        make install
      ${MHD_POST_INSTALL_COMMANDS}
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
