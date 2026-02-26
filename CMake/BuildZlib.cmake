include(AndroidThirdParty)

get_filename_component(TON_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TON_THIRD_PARTY_SOURCE_DIR "${TON_SOURCE_DIR}/third-party")
set(TON_THIRD_PARTY_BINARY_DIR "${CMAKE_BINARY_DIR}/third-party")

set(ZLIB_SOURCE_DIR ${TON_THIRD_PARTY_SOURCE_DIR}/zlib)
set(ZLIB_BINARY_DIR ${TON_THIRD_PARTY_BINARY_DIR}/zlib)
set(ZLIB_BUILD_DIR ${ZLIB_BINARY_DIR}/src)

if (ZLIB_FOUND)
  if (NOT ZLIB_LIBRARY AND ZLIB_LIBRARIES)
    list(GET ZLIB_LIBRARIES 0 ZLIB_LIBRARY)
  endif()
  if (NOT ZLIB_INCLUDE_DIR AND ZLIB_INCLUDE_DIRS)
    list(GET ZLIB_INCLUDE_DIRS 0 ZLIB_INCLUDE_DIR)
  endif()
  if (ZLIB_LIBRARY AND ZLIB_INCLUDE_DIR)
    if (NOT ZLIB_LIBRARY_DIRS)
      get_filename_component(ZLIB_LIBRARY_DIRS "${ZLIB_LIBRARY}" DIRECTORY)
    endif()
    set(ZLIB_LIBRARY ${ZLIB_LIBRARY} CACHE FILEPATH "Zlib library" FORCE)
    set(ZLIB_LIBRARIES ${ZLIB_LIBRARY} CACHE STRING "Zlib libraries" FORCE)
    set(ZLIB_INCLUDE_DIR ${ZLIB_INCLUDE_DIR} CACHE PATH "Zlib include dir" FORCE)
    set(ZLIB_INCLUDE_DIRS ${ZLIB_INCLUDE_DIR} CACHE PATH "Zlib include dir" FORCE)
    set(ZLIB_LIBRARY_DIRS ${ZLIB_LIBRARY_DIRS} CACHE PATH "Zlib library dir" FORCE)
    set(ZLIB_FOUND TRUE CACHE BOOL "Zlib found" FORCE)
    if (NOT TARGET zlib)
      add_custom_target(zlib DEPENDS ${ZLIB_LIBRARY})
    endif()
    message(STATUS "Using preconfigured zlib: ${ZLIB_LIBRARY}")
    return()
  endif()
endif()

if (MSVC)
  set(ZLIB_PROJECT_DIR ${ZLIB_SOURCE_DIR}/contrib/vstudio/vc14)
  set(ZLIB_LIBRARY ${ZLIB_BINARY_DIR}/lib/zlibstat.lib)
  set(ZLIB_INCLUDE_DIR ${ZLIB_SOURCE_DIR})
  file(MAKE_DIRECTORY ${ZLIB_BINARY_DIR}/lib)
  file(MAKE_DIRECTORY ${ZLIB_BINARY_DIR}/obj)
  file(TO_NATIVE_PATH "${ZLIB_BINARY_DIR}/lib/" ZLIB_MSVC_OUT_DIR)
  file(TO_NATIVE_PATH "${ZLIB_BINARY_DIR}/obj/" ZLIB_MSVC_INT_DIR)
  if (NOT EXISTS "${ZLIB_LIBRARY}")
    execute_process(
      COMMAND msbuild zlibstat.vcxproj /p:Configuration=ReleaseWithoutAsm /p:Platform=x64 -p:PlatformToolset=v142 /p:OutDir=${ZLIB_MSVC_OUT_DIR} /p:IntDir=${ZLIB_MSVC_INT_DIR}
      WORKING_DIRECTORY ${ZLIB_PROJECT_DIR}
      RESULT_VARIABLE ZLIB_BUILD_RESULT
    )
    if (NOT ZLIB_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "Zlib build failed with code ${ZLIB_BUILD_RESULT}")
    endif()
  endif()
  add_custom_command(
    WORKING_DIRECTORY ${ZLIB_PROJECT_DIR}
    COMMAND msbuild zlibstat.vcxproj /p:Configuration=ReleaseWithoutAsm /p:Platform=x64 -p:PlatformToolset=v142 /p:OutDir=${ZLIB_MSVC_OUT_DIR} /p:IntDir=${ZLIB_MSVC_INT_DIR}
    COMMENT "Build zlib (MSVC)"
    DEPENDS ${ZLIB_SOURCE_DIR}
    OUTPUT ${ZLIB_LIBRARY}
  )
else()
  if (ANDROID)
    set(ZLIB_BINARY_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/zlib/${TON_ANDROID_ARCH_DIR})
    set(ZLIB_BUILD_DIR ${ZLIB_BINARY_DIR}/src)
    set(ZLIB_CC ${TON_ANDROID_CC})
    set(ZLIB_AR ${TON_ANDROID_AR})
    set(ZLIB_RANLIB ${TON_ANDROID_RANLIB})
  else()
    set(ZLIB_CC ${CMAKE_C_COMPILER})
    set(ZLIB_AR ${CMAKE_AR})
    set(ZLIB_RANLIB ${CMAKE_RANLIB})
  endif()

  set(ZLIB_INCLUDE_DIR ${ZLIB_BINARY_DIR}/include)
  set(ZLIB_LIBRARY ${ZLIB_BINARY_DIR}/lib/libz.a)
  file(MAKE_DIRECTORY ${ZLIB_BINARY_DIR}/lib)
  file(MAKE_DIRECTORY ${ZLIB_INCLUDE_DIR})
  file(MAKE_DIRECTORY ${ZLIB_BUILD_DIR})

  if (CMAKE_C_FLAGS)
    set(ZLIB_CFLAGS "${CMAKE_C_FLAGS} -fPIC")
  else()
    set(ZLIB_CFLAGS "-fPIC")
  endif()

  if (MINGW)
    find_program(MSYS2_BASH bash)
    if (NOT MSYS2_BASH)
      message(FATAL_ERROR "bash not found in PATH; ensure MSYS2 is in PATH.")
    endif()
    add_custom_command(
      WORKING_DIRECTORY ${ZLIB_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${ZLIB_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${ZLIB_SOURCE_DIR} ${ZLIB_BUILD_DIR}
      COMMAND ${MSYS2_BASH} -lc "cd '${ZLIB_BUILD_DIR}' && CC='${ZLIB_CC}' AR='${ZLIB_AR}' RANLIB='${ZLIB_RANLIB}' CFLAGS='${ZLIB_CFLAGS}' ./configure --static --prefix='${ZLIB_BINARY_DIR}'"
      COMMAND ${MSYS2_BASH} -lc "cd '${ZLIB_BUILD_DIR}' && CC='${ZLIB_CC}' AR='${ZLIB_AR}' RANLIB='${ZLIB_RANLIB}' CFLAGS='${ZLIB_CFLAGS}' make clean"
      COMMAND ${MSYS2_BASH} -lc "cd '${ZLIB_BUILD_DIR}' && CC='${ZLIB_CC}' AR='${ZLIB_AR}' RANLIB='${ZLIB_RANLIB}' CFLAGS='${ZLIB_CFLAGS}' make -j16"
      COMMAND ${MSYS2_BASH} -lc "cd '${ZLIB_BUILD_DIR}' && CC='${ZLIB_CC}' AR='${ZLIB_AR}' RANLIB='${ZLIB_RANLIB}' CFLAGS='${ZLIB_CFLAGS}' make install"
      COMMENT "Build zlib"
      DEPENDS ${ZLIB_SOURCE_DIR}
      OUTPUT ${ZLIB_LIBRARY}
    )
  else()
    add_custom_command(
      WORKING_DIRECTORY ${ZLIB_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${ZLIB_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${ZLIB_SOURCE_DIR} ${ZLIB_BUILD_DIR}
      COMMAND ${CMAKE_COMMAND} -E rm -f ${ZLIB_LIBRARY}
      COMMAND ${CMAKE_COMMAND} -E chdir ${ZLIB_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${ZLIB_CC}
        AR=${ZLIB_AR}
        RANLIB=${ZLIB_RANLIB}
        CFLAGS=${ZLIB_CFLAGS}
        ./configure --static --prefix=${ZLIB_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E chdir ${ZLIB_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${ZLIB_CC}
        AR=${ZLIB_AR}
        RANLIB=${ZLIB_RANLIB}
        CFLAGS=${ZLIB_CFLAGS}
        make clean
      COMMAND ${CMAKE_COMMAND} -E chdir ${ZLIB_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${ZLIB_CC}
        AR=${ZLIB_AR}
        RANLIB=${ZLIB_RANLIB}
        CFLAGS=${ZLIB_CFLAGS}
        make -j16
      COMMAND ${CMAKE_COMMAND} -E chdir ${ZLIB_BUILD_DIR} ${CMAKE_COMMAND} -E env
        CC=${ZLIB_CC}
        AR=${ZLIB_AR}
        RANLIB=${ZLIB_RANLIB}
        CFLAGS=${ZLIB_CFLAGS}
        make install
      COMMAND ${ZLIB_RANLIB} ${ZLIB_LIBRARY}
      COMMENT "Build zlib"
      DEPENDS ${ZLIB_SOURCE_DIR}
      OUTPUT ${ZLIB_LIBRARY}
    )
  endif()
endif()

set(ZLIB_LIBRARY ${ZLIB_LIBRARY} CACHE FILEPATH "Zlib library" FORCE)
set(ZLIB_LIBRARIES ${ZLIB_LIBRARY} CACHE STRING "Zlib libraries" FORCE)
set(ZLIB_INCLUDE_DIR ${ZLIB_INCLUDE_DIR} CACHE PATH "Zlib include dir" FORCE)
set(ZLIB_INCLUDE_DIRS ${ZLIB_INCLUDE_DIR} CACHE PATH "Zlib include dir" FORCE)
if (NOT ZLIB_LIBRARY_DIRS)
  get_filename_component(ZLIB_LIBRARY_DIRS "${ZLIB_LIBRARY}" DIRECTORY)
endif()
set(ZLIB_LIBRARY_DIRS ${ZLIB_LIBRARY_DIRS} CACHE PATH "Zlib library dir" FORCE)
set(ZLIB_FOUND TRUE CACHE BOOL "Zlib found" FORCE)

add_custom_target(zlib DEPENDS ${ZLIB_LIBRARY})
