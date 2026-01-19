set(BLST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/blst)
set(BLST_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/blst)
set(BLST_INCLUDE_DIR ${BLST_SOURCE_DIR}/bindings)

if (NOT BLST_LIB)
  if (WIN32)
    set(BLST_LIB ${BLST_BINARY_DIR}/blst.lib)
    set(BLST_BUILD_COMMAND ${BLST_SOURCE_DIR}/build.bat)
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
  else()
    add_custom_command(
      WORKING_DIRECTORY ${BLST_BINARY_DIR}
      COMMAND ${BLST_BUILD_COMMAND}
      COMMENT "Build blst"
      DEPENDS ${BLST_SOURCE_DIR}
      OUTPUT ${BLST_LIB}
    )
  endif()
else()
  message(STATUS "Use BLST: ${BLST_LIB}")
endif()

add_custom_target(blst DEPENDS ${BLST_LIB})
