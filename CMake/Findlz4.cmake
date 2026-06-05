include(FindPackageHandleStandardArgs)

if (NOT LZ4_LIBRARIES OR NOT LZ4_INCLUDE_DIRS)
  if (LZ4_SOURCE_DIR)
    set(LZ4_INCLUDE_HINTS ${LZ4_SOURCE_DIR}/lib)
    set(LZ4_LIBRARY_HINTS ${LZ4_BINARY_DIR}/lib)
  else()
    set(LZ4_INCLUDE_HINTS)
    set(LZ4_LIBRARY_HINTS)
  endif()

  find_path(LZ4_INCLUDE_DIRS
    NAMES lz4.h
    PATHS ${LZ4_INCLUDE_HINTS}
    NO_DEFAULT_PATH
  )
  find_library(LZ4_LIBRARIES
    NAMES lz4 lz4_static
    PATHS ${LZ4_LIBRARY_HINTS}
    NO_DEFAULT_PATH
  )
endif()

set(LZ4_INCLUDE_DIR ${LZ4_INCLUDE_DIRS})
set(LZ4_LIBRARY ${LZ4_LIBRARIES})

find_package_handle_standard_args(lz4 REQUIRED_VARS LZ4_LIBRARIES LZ4_INCLUDE_DIRS)
set(LZ4_FOUND ${lz4_FOUND})

if (lz4_FOUND AND NOT TARGET lz4::lz4)
  add_library(lz4::lz4 UNKNOWN IMPORTED)
  set_target_properties(lz4::lz4 PROPERTIES
    IMPORTED_LOCATION "${LZ4_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIRS}"
  )
  if (TARGET lz4)
    add_dependencies(lz4::lz4 lz4)
  endif()
endif()
