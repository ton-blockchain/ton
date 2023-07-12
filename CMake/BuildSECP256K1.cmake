if (NOT SECP256K1_LIBRARY)

    set(SECP256K1_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/secp256k1)
    set(SECP256K1_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/secp256k1)
    set(SECP256K1_INCLUDE_DIR ${SECP256K1_BINARY_DIR}/include)

    file(MAKE_DIRECTORY ${SECP256K1_BINARY_DIR})

    if (WIN32)
      set(SECP256K1_LIBRARY ${SECP256K1_SOURCE_DIR}/bin/x64/Release/v142/static/secp256k1.lib)
      add_custom_command(
        WORKING_DIRECTORY ${SECP256K1_SOURCE_DIR}
        COMMAND cd builds\msvc\vs2017
        COMMAND msbuild /p:Configuration=StaticRelease -p:PlatformToolset=v142 -p:Platform=x64
        COMMENT "Build secp256k1"
        DEPENDS ${SECP256K1_SOURCE_DIR}
        OUTPUT ${SECP256K1_LIBRARY}
      )
    else()
      set(SECP256K1_LIBRARY ${SECP256K1_BINARY_DIR}/lib/libsecp256k1.a)
      add_custom_command(
          WORKING_DIRECTORY ${SECP256K1_SOURCE_DIR}
          COMMAND ./autogen.sh
          COMMAND ./configure --disable-option-checking --enable-module-recovery --prefix ${SECP256K1_BINARY_DIR} --with-pic --disable-shared --enable-static --disable-tests --disable-benchmark
          COMMAND make
          COMMAND make install
          COMMENT "Build secp256k1"
          DEPENDS ${SECP256K1_SOURCE_DIR}
          OUTPUT ${SECP256K1_LIBRARY}
      )
    endif()

else()
   message(STATUS "Use secp256k1: ${SECP256K1_LIBRARY}")
endif()

add_custom_target(secp256k1 DEPENDS ${SECP256K1_LIBRARY})
