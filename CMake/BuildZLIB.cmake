cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT ZLIB_LIBRARY)

    set(ZLIB_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/zlib)
    set(ZLIB_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/zlib)
    set(ZLIB_INCLUDE_DIR ${ZLIB_BINARY_DIR}/include)

    file(MAKE_DIRECTORY ${ZLIB_BINARY_DIR})

    if (MSVC)
      set(ZLIB_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/zlib)
      set(ZLIB_LIBRARY ${ZLIB_SOURCE_DIR}/contrib/vstudio/vc14/x64/ZlibStatReleaseWithoutAsm/zlibstat.lib)
      set(ZLIB_INCLUDE_DIR ${ZLIB_BINARY_DIR})
      add_custom_command(
        WORKING_DIRECTORY ${ZLIB_SOURCE_DIR}
        COMMAND cd contrib/vstudio/vc14
        COMMAND msbuild /m /v:n zlibstat.vcxproj /p:Configuration=ReleaseWithoutAsm /p:platform=x64 -p:PlatformToolset=v142
        COMMENT "Build zlib with MSVC"
        DEPENDS ${ZLIB_SOURCE_DIR}
        OUTPUT ${ZLIB_LIBRARY}
      )
    elseif (EMSCRIPTEN)
      set(ZLIB_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/zlib)
      set(ZLIB_LIBRARY ${ZLIB_BINARY_DIR}/libz.a)
      set(ZLIB_INCLUDE_DIR ${ZLIB_BINARY_DIR})
      add_custom_command(
          WORKING_DIRECTORY ${ZLIB_SOURCE_DIR}
          COMMAND emconfigure ./configure --static
          COMMAND emmake make clean
          COMMAND emmake make
          COMMENT "Build zlib with emscripten"
          DEPENDS ${ZLIB_SOURCE_DIR}
          OUTPUT ${ZLIB_LIBRARY}
      )
    else()
      set(ZLIB_LIBRARY ${ZLIB_BINARY_DIR}/lib/libz.a)
      add_custom_command(
          WORKING_DIRECTORY ${ZLIB_SOURCE_DIR}
          COMMAND ./configure --static --prefix ${ZLIB_BINARY_DIR}
          COMMAND make clean
          COMMAND make -j16
          COMMAND make install
          COMMENT "Build zlib"
          DEPENDS ${ZLIB_SOURCE_DIR}
          OUTPUT ${ZLIB_LIBRARY}
      )
    endif()

else()
   message(STATUS "Use zlib: ${ZLIB_LIBRARY}")
endif()

add_custom_target(zlib DEPENDS ${ZLIB_LIBRARY})
