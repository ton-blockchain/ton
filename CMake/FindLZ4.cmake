###############################################################################
# Find LZ4
#
# This sets the following variables:
# LZ4_FOUND        - True if LZ4 was found.
# LZ4_INCLUDE_DIRS - Directories containing the LZ4 include files.
# LZ4_LIBRARIES    - Libraries needed to use LZ4.
# LZ4_LIBRARY      - Library needed to use LZ4.
# LZ4_LIBRARY_DIRS - Library needed to use LZ4.

find_package(PkgConfig REQUIRED)

# If found, LZ$_* variables will be defined
pkg_check_modules(LZ4 REQUIRED liblz4)

if(NOT LZ4_FOUND)
  find_path(LZ4_INCLUDE_DIR lz4.h
    HINTS "${LZ4_ROOT}" "$ENV{LZ4_ROOT}"
    PATHS "$ENV{PROGRAMFILES}/lz4" "$ENV{PROGRAMW6432}/lz4"
    PATH_SUFFIXES include)

  find_library(LZ4_LIBRARY
    NAMES lz4 lz4_static
    HINTS "${LZ4_ROOT}" "$ENV{LZ4_ROOT}"
    PATHS "$ENV{PROGRAMFILES}/lz4" "$ENV{PROGRAMW6432}/lz4"
    PATH_SUFFIXES lib)

  if(LZ4_LIBRARY)
    set(LZ4_LIBRARIES ${LZ4_LIBRARY})
    get_filename_component(LZ4_LIBRARY_DIRS ${LZ4_LIBRARY} DIRECTORY)
  endif()
else()
  find_library(LZ4_LIBRARY
    NAMES lz4 lz4_static
    PATHS ${LZ4_LIBRARY_DIRS}
    NO_DEFAULT_PATH)
endif()

mark_as_advanced(LZ4_LIBRARY LZ4_INCLUDE_DIRS LZ4_LIBRARY_DIRS LZ4_LIBRARIES)