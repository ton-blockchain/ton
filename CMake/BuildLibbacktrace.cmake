# Build libbacktrace from source
# libbacktrace is a library from GCC for getting backtraces with file and line information
#
# Usage options:
#   1. Use pre-built library:
#      set(LIBBACKTRACE_LIBRARY "/path/to/libbacktrace.a")
#      set(LIBBACKTRACE_INCLUDE_DIR "/path/to/include")
#   2. Build from submodule (default): uses third-party/libbacktrace

include(ExternalProject)

# Skip libbacktrace on Android - it doesn't work well with Android's unwinder
if(ANDROID)
  message(STATUS "Skipping libbacktrace on Android (not supported)")
  set(LIBBACKTRACE_FOUND FALSE CACHE BOOL "libbacktrace found" FORCE)
  return()
endif()

# Check if user provided pre-built library
if(LIBBACKTRACE_LIBRARY AND LIBBACKTRACE_INCLUDE_DIR)
  if(EXISTS "${LIBBACKTRACE_LIBRARY}" AND EXISTS "${LIBBACKTRACE_INCLUDE_DIR}/backtrace.h")
    message(STATUS "Using pre-built libbacktrace: ${LIBBACKTRACE_LIBRARY}")
    add_library(libbacktrace STATIC IMPORTED GLOBAL)
    set_target_properties(libbacktrace PROPERTIES
      IMPORTED_LOCATION "${LIBBACKTRACE_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBBACKTRACE_INCLUDE_DIR}"
    )
    set(LIBBACKTRACE_FOUND TRUE CACHE BOOL "libbacktrace found" FORCE)
    return()
  endif()
endif()

# Set default paths for building
if(NOT LIBBACKTRACE_ROOT_DIR)
  set(LIBBACKTRACE_ROOT_DIR "${CMAKE_CURRENT_BINARY_DIR}/libbacktrace")
endif()

if(NOT LIBBACKTRACE_INCLUDE_DIR)
  set(LIBBACKTRACE_INCLUDE_DIR "${LIBBACKTRACE_ROOT_DIR}/include")
endif()

if(NOT LIBBACKTRACE_LIBRARY)
  set(LIBBACKTRACE_LIBRARY "${LIBBACKTRACE_ROOT_DIR}/lib/libbacktrace.a")
endif()

# Create directories early to avoid CMake errors
file(MAKE_DIRECTORY "${LIBBACKTRACE_INCLUDE_DIR}")
file(MAKE_DIRECTORY "${LIBBACKTRACE_ROOT_DIR}/lib")

# Use submodule path if not overridden
if(NOT LIBBACKTRACE_SOURCE_DIR)
  set(LIBBACKTRACE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/libbacktrace")
endif()

message(STATUS "Building libbacktrace from: ${LIBBACKTRACE_SOURCE_DIR}")

# Build configure arguments - handle cross-compilation
set(LIBBACKTRACE_CONFIGURE_ARGS
  --prefix=<INSTALL_DIR>
  --enable-static
  --disable-shared
  --with-pic
)

# For cross-compilation, pass the host triplet
if(CMAKE_CROSSCOMPILING AND CMAKE_C_COMPILER_TARGET)
  list(APPEND LIBBACKTRACE_CONFIGURE_ARGS --host=${CMAKE_C_COMPILER_TARGET})
endif()

# Set up environment for configure
set(LIBBACKTRACE_ENV
  CC=${CMAKE_C_COMPILER}
)

# For cross-compilation, also set AR and RANLIB if available
if(CMAKE_AR)
  list(APPEND LIBBACKTRACE_ENV AR=${CMAKE_AR})
endif()
if(CMAKE_RANLIB)
  list(APPEND LIBBACKTRACE_ENV RANLIB=${CMAKE_RANLIB})
endif()

# Pass C flags (important for cross-compilation sysroot, etc.)
if(CMAKE_C_FLAGS)
  list(APPEND LIBBACKTRACE_ENV "CFLAGS=${CMAKE_C_FLAGS}")
endif()

ExternalProject_Add(
  libbacktrace_external
  PREFIX "${LIBBACKTRACE_ROOT_DIR}"
  SOURCE_DIR "${LIBBACKTRACE_SOURCE_DIR}"
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${LIBBACKTRACE_ENV}
    <SOURCE_DIR>/configure ${LIBBACKTRACE_CONFIGURE_ARGS}
  BUILD_COMMAND make
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS "${LIBBACKTRACE_LIBRARY}"
)

# Create imported target
add_library(libbacktrace STATIC IMPORTED GLOBAL)
set_target_properties(libbacktrace PROPERTIES
  IMPORTED_LOCATION "${LIBBACKTRACE_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${LIBBACKTRACE_INCLUDE_DIR}"
)

# Make libbacktrace depend on the external project
add_dependencies(libbacktrace libbacktrace_external)

set(LIBBACKTRACE_FOUND TRUE CACHE BOOL "libbacktrace found" FORCE)

