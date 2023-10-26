# - Try to find MHD
# Once done this will define
#
#  MHD_FOUND - system has MHD
#  MHD_INCLUDE_DIRS - the MHD include directory
#  MHD_LIBRARY - Link these to use MHD

find_path(
    MHD_INCLUDE_DIR
    NAMES microhttpd.h
    DOC "microhttpd include dir"
)

find_library(
    MHD_LIBRARY
    NAMES microhttpd microhttpd-10 libmicrohttpd libmicrohttpd-dll
    DOC "microhttpd library"
)

set(MHD_INCLUDE_DIRS ${MHD_INCLUDE_DIR})
set(MHD_LIBRARIES ${MHD_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MHD DEFAULT_MSG MHD_INCLUDE_DIR MHD_LIBRARY)
mark_as_advanced(MHD_INCLUDE_DIR MHD_LIBRARY)
