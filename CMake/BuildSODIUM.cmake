set(SODIUM_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sodium)
set(SODIUM_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/sodium)
set(SODIUM_INCLUDE_DIR ${SODIUM_BINARY_DIR}/include)

if (NOT SODIUM_LIBRARY_RELEASE)
    if (WIN32)
      set(SODIUM_LIBRARY_RELEASE ${SODIUM_BINARY_DIR}/lib/libsodium.lib)
    else()
      set(SODIUM_LIBRARY_RELEASE ${SODIUM_BINARY_DIR}/lib/libsodium.so)
    endif()

    file(MAKE_DIRECTORY ${SODIUM_BINARY_DIR})

    add_custom_command(
      WORKING_DIRECTORY ${SODIUM_SOURCE_DIR}
      COMMAND export LIBSODIUM_FULL_BUILD=1
      COMMAND ./autogen.sh
      COMMAND ./configure --prefix ${SODIUM_BINARY_DIR}
      COMMAND make
      COMMAND make install
      COMMENT "Build sodium"
      DEPENDS ${SODIUM_SOURCE_DIR}
      OUTPUT ${SODIUM_LIBRARY_RELEASE}
    )
else()
   message(STATUS "Use sodium: ${SODIUM_LIBRARY_RELEASE}")
endif()

add_custom_target(sodium DEPENDS ${SODIUM_LIBRARY_RELEASE})
