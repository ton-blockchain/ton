# Custom Findlz4.cmake that respects pre-existing lz4::lz4 target
# This prevents RocksDB's Findlz4.cmake from running find_library
# which finds NuGet packages with malformed paths on Windows CI

# If lz4::lz4 target already exists, just set found and return
if(TARGET lz4::lz4)
  set(lz4_FOUND TRUE)
  # Get properties from existing target for compatibility
  get_target_property(lz4_LIBRARIES lz4::lz4 IMPORTED_LOCATION)
  get_target_property(lz4_INCLUDE_DIRS lz4::lz4 INTERFACE_INCLUDE_DIRECTORIES)
  return()
endif()

# Skip find_library on Windows - finds NuGet with malformed paths
# On Windows, LZ4 should be provided via cmake args from build script
if(WIN32)
  set(lz4_FOUND FALSE)
  return()
endif()

# Otherwise, fall back to standard detection (non-Windows only)
find_path(lz4_INCLUDE_DIRS
  NAMES lz4.h
  HINTS ${lz4_ROOT_DIR}/include)

find_library(lz4_LIBRARIES
  NAMES lz4
  HINTS ${lz4_ROOT_DIR}/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(lz4 DEFAULT_MSG lz4_LIBRARIES lz4_INCLUDE_DIRS)

mark_as_advanced(
  lz4_LIBRARIES
  lz4_INCLUDE_DIRS)

if(lz4_FOUND AND NOT (TARGET lz4::lz4))
  add_library(lz4::lz4 UNKNOWN IMPORTED GLOBAL)
  set_target_properties(lz4::lz4
    PROPERTIES
      IMPORTED_LOCATION ${lz4_LIBRARIES}
      INTERFACE_INCLUDE_DIRECTORIES ${lz4_INCLUDE_DIRS})
endif()
