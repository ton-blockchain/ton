cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

set(MHD_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/mhd)
set(MHD_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/mhd)
set(MHD_INCLUDE_DIR ${MHD_BINARY_DIR}/include)

if (NOT MHD_LIBRARY)
    if (WIN32)
      set(MHD_LIBRARY ${MHD_BINARY_DIR}/lib/libmicrohttpd.lib)
    else()
      set(MHD_LIBRARY ${MHD_BINARY_DIR}/lib/libmicrohttpd.a)
    endif()

    file(MAKE_DIRECTORY ${MHD_BINARY_DIR})

    add_custom_command(
      WORKING_DIRECTORY ${MHD_SOURCE_DIR}
      COMMAND ./autogen.sh
      COMMAND ./configure --disable-option-checking --prefix ${MHD_BINARY_DIR} --with-pic --disable-shared --enable-static --without-gnutls
      COMMAND make
      COMMAND make install
      COMMENT "Build mhd"
      DEPENDS ${MHD_SOURCE_DIR}
      OUTPUT ${MHD_LIBRARY}
    )
else()
   message(STATUS "Use mhd: ${MHD_LIBRARY}")
endif()

add_custom_target(mhd DEPENDS ${MHD_LIBRARY})
