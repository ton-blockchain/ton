set(SECP256K1_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/secp256k1)
set(SECP256K1_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/secp256k1)
set(SECP256K1_INCLUDE_DIR ${SECP256K1_BINARY_DIR}/include)

if (NOT SECP256K1_LIB)
    if (WIN32)
      set(SECP256K1_LIB ${SECP256K1_BINARY_DIR}/lib/libsecp256k1.lib)
    else()
      set(SECP256K1_LIB ${SECP256K1_BINARY_DIR}/lib/libsecp256k1.a)
    endif()

    file(MAKE_DIRECTORY ${SECP256K1_BINARY_DIR})

    add_custom_command(
      WORKING_DIRECTORY ${SECP256K1_SOURCE_DIR}
      COMMAND ./autogen.sh
      COMMAND ./configure --enable-module-recovery --prefix ${SECP256K1_BINARY_DIR}
      COMMAND make
      COMMAND make install
      COMMENT "Build secp256k1"
      DEPENDS ${SECP256K1_SOURCE_DIR}
      OUTPUT ${SECP256K1_LIB}
    )
else()
   message(STATUS "Use secp256k1: ${SECP256K1_LIB}")
endif()

add_custom_target(secp256k1 DEPENDS ${SECP256K1_LIB})
