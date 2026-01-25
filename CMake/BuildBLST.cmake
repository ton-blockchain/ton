include(AndroidThirdParty)

set(BLST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/blst)
set(BLST_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/blst)
set(BLST_INCLUDE_DIR ${BLST_SOURCE_DIR}/bindings)

if (NOT BLST_LIB)
  if (ANDROID)
    set(BLST_BINARY_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/blst/${TON_ANDROID_ARCH_DIR})
    set(BLST_LIB ${BLST_BINARY_DIR}/libblst.a)
    set(BLST_AR ${TON_ANDROID_AR})
    set(BLST_RANLIB ${TON_ANDROID_RANLIB})
    if (CMAKE_C_FLAGS)
      set(BLST_CFLAGS "${CMAKE_C_FLAGS} -fPIC")
    else()
      set(BLST_CFLAGS "-fPIC")
    endif()
  elseif (WIN32)
    if (MINGW)
      set(BLST_LIB ${BLST_BINARY_DIR}/libblst.a)
      if (PORTABLE)
        set(BLST_BUILD_COMMAND ${BLST_SOURCE_DIR}/build.sh -D__BLST_PORTABLE__)
      else()
        set(BLST_BUILD_COMMAND ${BLST_SOURCE_DIR}/build.sh)
      endif()
      set(BLST_BUILD_SHELL "C:/msys64/usr/bin/bash.exe")
    else()
      set(BLST_LIB ${BLST_BINARY_DIR}/blst.lib)
      set(BLST_BUILD_COMMAND ${BLST_SOURCE_DIR}/build.bat)
    endif()
  else()
    set(BLST_LIB ${BLST_BINARY_DIR}/libblst.a)
    if (PORTABLE)
      set(BLST_BUILD_COMMAND ${BLST_SOURCE_DIR}/build.sh -D__BLST_PORTABLE__)
    else()
      set(BLST_BUILD_COMMAND ${BLST_SOURCE_DIR}/build.sh)
    endif()
  endif()

  file(MAKE_DIRECTORY ${BLST_BINARY_DIR})
  if (USE_EMSCRIPTEN OR EMSCRIPTEN)
    set(BLST_CFLAGS -O2 -fno-builtin -fPIC -Wall -Wextra -Werror -D__BLST_NO_ASM__)
    add_custom_command(
      WORKING_DIRECTORY ${BLST_BINARY_DIR}
      COMMAND emcc ${BLST_CFLAGS} -c ${BLST_SOURCE_DIR}/src/server.c -o server.o
      COMMAND emar rcs ${BLST_LIB} server.o
      COMMAND emranlib ${BLST_LIB}
      COMMENT "Build blst (emscripten)"
      DEPENDS ${BLST_SOURCE_DIR}/src/server.c
      OUTPUT ${BLST_LIB}
    )
  elseif (ANDROID)
    add_custom_command(
      WORKING_DIRECTORY ${BLST_BINARY_DIR}
      COMMAND ${CMAKE_COMMAND} -E env
        CC=${TON_ANDROID_CC}
        AR=${BLST_AR}
        RANLIB=${BLST_RANLIB}
        CFLAGS=${BLST_CFLAGS}
        ${BLST_SOURCE_DIR}/build.sh
      COMMENT "Build blst (Android)"
      DEPENDS ${BLST_SOURCE_DIR}
      OUTPUT ${BLST_LIB}
    )
  else()
    if (MINGW)
      add_custom_command(
        WORKING_DIRECTORY ${BLST_BINARY_DIR}
        COMMAND ${BLST_BUILD_SHELL} -lc "${BLST_BUILD_COMMAND}"
        COMMENT "Build blst"
        DEPENDS ${BLST_SOURCE_DIR}
        OUTPUT ${BLST_LIB}
      )
    else()
      add_custom_command(
        WORKING_DIRECTORY ${BLST_BINARY_DIR}
        COMMAND ${BLST_BUILD_COMMAND}
        COMMENT "Build blst"
        DEPENDS ${BLST_SOURCE_DIR}
        OUTPUT ${BLST_LIB}
      )
    endif()
  endif()
else()
  message(STATUS "Use BLST: ${BLST_LIB}")
endif()

add_custom_target(blst DEPENDS ${BLST_LIB})
