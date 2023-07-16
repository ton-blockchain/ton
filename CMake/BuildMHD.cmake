cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT MHD_LIBRARY)

set(MHD_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/mhd)
set(MHD_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/mhd)
set(MHD_INCLUDE_DIR ${MHD_BINARY_DIR}/include)

    file(MAKE_DIRECTORY ${MHD_BINARY_DIR})

    if (MSVC)
      set(MHD_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/mhd)
      set(MHD_LIBRARY ${MHD_SOURCE_DIR}/w32/VS2017/Output/x64/libmicrohttpd.lib)
      set(MHD_INCLUDE_DIR ${MHD_BINARY_DIR}/src/include)
      add_custom_command(
        WORKING_DIRECTORY ${MHD_SOURCE_DIR}
        COMMAND cd w32/VS2017
        COMMAND msbuild /m /v:n /p:Configuration=Release-static -p:PlatformToolset=v142 -p:Platform=x64 libmicrohttpd.sln
        COMMENT "Build mhd with MSVC"
        DEPENDS ${MHD_SOURCE_DIR}
        OUTPUT ${MHD_LIBRARY}
      )
    else()
      set(MHD_LIBRARY ${MHD_BINARY_DIR}/lib/libmicrohttpd.a)
      add_custom_command(
        WORKING_DIRECTORY ${MHD_SOURCE_DIR}
        COMMAND ./autogen.sh
        COMMAND ./configure -q --disable-option-checking --prefix ${MHD_BINARY_DIR} --with-pic --disable-shared --enable-static --without-gnutls
        COMMAND make clean
        COMMAND make -j16
        COMMAND make install
        COMMENT "Build mhd"
        DEPENDS ${MHD_SOURCE_DIR}
        OUTPUT ${MHD_LIBRARY}
      )
    endif()
else()
   message(STATUS "Use mhd: ${MHD_LIBRARY}")
endif()

add_custom_target(mhd DEPENDS ${MHD_LIBRARY})
