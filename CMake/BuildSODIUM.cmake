cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT SODIUM_LIBRARY_RELEASE)

    set(SODIUM_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sodium)
    set(SODIUM_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/sodium)
    set(SODIUM_INCLUDE_DIR ${SODIUM_BINARY_DIR}/include)

    file(MAKE_DIRECTORY ${SODIUM_BINARY_DIR})

    if (MSVC)
      set(SODIUM_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sodium)
      set(SODIUM_LIBRARY_RELEASE ${SODIUM_SOURCE_DIR}/bin/x64/Release/v142/static/libsodium.lib)
      set(SODIUM_INCLUDE_DIR ${SODIUM_BINARY_DIR}/src/libsodium/include)
      add_custom_command(
        WORKING_DIRECTORY ${SODIUM_SOURCE_DIR}
        COMMAND set LIBSODIUM_FULL_BUILD=1
        COMMAND cd builds/msvc/vs2017
        COMMAND msbuild /m /v:n /p:Configuration=StaticRelease -p:PlatformToolset=v142 -p:Platform=x64
        COMMENT "Build sodium with vs2017"
        DEPENDS ${SODIUM_SOURCE_DIR}
        OUTPUT ${SODIUM_LIBRARY_RELEASE}
      )
    elseif (EMSCRIPTEN)
      set(SODIUM_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/sodium)
      set(SODIUM_LIBRARY_RELEASE ${SODIUM_BINARY_DIR}/src/libsodium/.libs/libsodium.a)
      set(SODIUM_INCLUDE_DIR ${SODIUM_BINARY_DIR}/src/libsodium/include)
      add_custom_command(
        WORKING_DIRECTORY ${SODIUM_SOURCE_DIR}
        COMMAND export LIBSODIUM_FULL_BUILD=1
        COMMAND ./autogen.sh
        COMMAND emconfigure ./configure --disable-ssp
        COMMAND emmake make
        COMMENT "Build sodium with emscripten"
        DEPENDS ${SODIUM_SOURCE_DIR}
        OUTPUT ${SODIUM_LIBRARY_RELEASE}
      )
    else()
      set(SODIUM_LIBRARY_RELEASE ${SODIUM_BINARY_DIR}/lib/libsodium.a)
      add_custom_command(
        WORKING_DIRECTORY ${SODIUM_SOURCE_DIR}
        COMMAND export LIBSODIUM_FULL_BUILD=1
        COMMAND ./autogen.sh
        COMMAND ./configure --prefix ${SODIUM_BINARY_DIR} --disable-ssp
        COMMAND make
        COMMAND make install
        COMMENT "Build sodium"
        DEPENDS ${SODIUM_SOURCE_DIR}
        OUTPUT ${SODIUM_LIBRARY_RELEASE}
      )
    endif()

else()
   message(STATUS "Use sodium: ${SODIUM_LIBRARY_RELEASE}")
endif()

add_custom_target(sodium DEPENDS ${SODIUM_LIBRARY_RELEASE})
