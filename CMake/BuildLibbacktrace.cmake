# Build libbacktrace from source
# libbacktrace is a library from GCC for getting backtraces with file and line information
#
# Usage options:
#   1. Use pre-built library:
#      set(LIBBACKTRACE_LIBRARY "/path/to/libbacktrace.a")
#      set(LIBBACKTRACE_INCLUDE_DIR "/path/to/include")
#   2. Build from submodule:
#      set(LIBBACKTRACE_SOURCE_DIR "/path/to/libbacktrace/source")
#   3. Auto-download from GitHub (default if nothing is set)

include(ExternalProject)

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

# Check if source path is provided (e.g., git submodule)
if(LIBBACKTRACE_SOURCE_DIR)
  message(STATUS "Building libbacktrace from: ${LIBBACKTRACE_SOURCE_DIR}")
  
  ExternalProject_Add(
    libbacktrace_external
    PREFIX "${LIBBACKTRACE_ROOT_DIR}"
    SOURCE_DIR "${LIBBACKTRACE_SOURCE_DIR}"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
      CC=${CMAKE_C_COMPILER}
      <SOURCE_DIR>/configure
      --prefix=<INSTALL_DIR>
      --enable-static
      --disable-shared
      --with-pic
    BUILD_COMMAND make
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS "${LIBBACKTRACE_LIBRARY}"
  )
else()
  message(STATUS "Building libbacktrace from GitHub")
  
  ExternalProject_Add(
    libbacktrace_external
    PREFIX "${LIBBACKTRACE_ROOT_DIR}"
    GIT_REPOSITORY https://github.com/ianlancetaylor/libbacktrace.git
    GIT_TAG b9e40069c0b47a722286b94eb5231f7f05c08713
    GIT_SHALLOW TRUE
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
      CC=${CMAKE_C_COMPILER}
      <SOURCE_DIR>/configure
      --prefix=<INSTALL_DIR>
      --enable-static
      --disable-shared
      --with-pic
    BUILD_COMMAND make
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS "${LIBBACKTRACE_LIBRARY}"
  )
endif()

# Create imported target
add_library(libbacktrace STATIC IMPORTED GLOBAL)
set_target_properties(libbacktrace PROPERTIES
  IMPORTED_LOCATION "${LIBBACKTRACE_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${LIBBACKTRACE_INCLUDE_DIR}"
)

# Make libbacktrace depend on the external project
add_dependencies(libbacktrace libbacktrace_external)

set(LIBBACKTRACE_FOUND TRUE CACHE BOOL "libbacktrace found" FORCE)

