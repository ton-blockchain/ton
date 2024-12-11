# - Try to find Secp256k1
# Once done this will define
#
#  SECP256K1_INCLUDE_DIR - the Secp256k1 include directory
#  SECP256K1_LIBRARY - Link these to use Secp256k1

if (NOT SECP256K1_LIBRARY)
    find_path(
        SECP256K1_INCLUDE_DIR
        NAMES secp256k1_recovery.h
        DOC "secp256k1_recovery.h include dir"
    )

    find_library(
        SECP256K1_LIBRARY
        NAMES secp256k1 libsecp256k1
        DOC "secp256k1 library"
    )
endif()

if (SECP256K1_LIBRARY)
  message(STATUS "Found Secp256k1: ${SECP256K1_LIBRARY}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Secp256k1 DEFAULT_MSG SECP256K1_INCLUDE_DIR SECP256K1_LIBRARY)
mark_as_advanced(SECP256K1_INCLUDE_DIR SECP256K1_LIBRARY)
