include(AndroidThirdParty)

if (NOT SECP256K1_LIBRARY)

    set(SECP256K1_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/secp256k1)
    set(SECP256K1_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/secp256k1)
    set(SECP256K1_INCLUDE_DIR ${SECP256K1_BINARY_DIR}/include)

    file(MAKE_DIRECTORY ${SECP256K1_BINARY_DIR})
    file(MAKE_DIRECTORY "${SECP256K1_BINARY_DIR}/include")

    if (MSVC)
      set(SECP256K1_BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/secp256k1)
      set(SECP256K1_LIBRARY ${SECP256K1_SOURCE_DIR}/build/src/Release/libsecp256k1.lib)
      set(SECP256K1_INCLUDE_DIR ${SECP256K1_BINARY_DIR}/include)
      add_custom_command(
        WORKING_DIRECTORY ${SECP256K1_SOURCE_DIR}
        COMMAND cmake -E env CFLAGS="/WX" cmake -A x64 -B build -DSECP256K1_ENABLE_MODULE_RECOVERY=ON -DSECP256K1_ENABLE_MODULE_EXTRAKEYS=ON -DSECP256K1_BUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
        COMMAND cmake --build build --config Release
        COMMENT "Build Secp256k1"
        DEPENDS ${SECP256K1_SOURCE_DIR}
        OUTPUT ${SECP256K1_LIBRARY}
      )
    elseif (EMSCRIPTEN)
      set(SECP256K1_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/secp256k1-emscripten)
      set(SECP256K1_EMSRC_DIR ${CMAKE_CURRENT_BINARY_DIR}/third-party/secp256k1-src-emscripten)
      set(SECP256K1_LIBRARY ${SECP256K1_BINARY_DIR}/lib/libsecp256k1.a)
      set(SECP256K1_INCLUDE_DIR ${SECP256K1_BINARY_DIR}/include)
      set(SECP256K1_LIBRARY ${SECP256K1_LIBRARY} CACHE FILEPATH "Secp256k1 library" FORCE)
      set(SECP256K1_INCLUDE_DIR ${SECP256K1_INCLUDE_DIR} CACHE PATH "Secp256k1 include dir" FORCE)
      if (CMAKE_AR)
        set(SECP256K1_AR ${CMAKE_AR})
      else()
        set(SECP256K1_AR emar)
      endif()
      if (CMAKE_RANLIB)
        set(SECP256K1_RANLIB ${CMAKE_RANLIB})
      else()
        set(SECP256K1_RANLIB emranlib)
      endif()
      file(MAKE_DIRECTORY ${SECP256K1_BINARY_DIR})
      add_custom_command(
          WORKING_DIRECTORY ${SECP256K1_SOURCE_DIR}
          COMMAND ${CMAKE_COMMAND} -E rm -rf ${SECP256K1_EMSRC_DIR}
          COMMAND ${CMAKE_COMMAND} -E copy_directory ${SECP256K1_SOURCE_DIR} ${SECP256K1_EMSRC_DIR}
          COMMAND ${CMAKE_COMMAND} -E chdir ${SECP256K1_EMSRC_DIR} ./autogen.sh
          COMMAND ${CMAKE_COMMAND} -E env
            CC=${CMAKE_C_COMPILER}
            CXX=${CMAKE_CXX_COMPILER}
            AR=${SECP256K1_AR}
            RANLIB=${SECP256K1_RANLIB}
            ${CMAKE_COMMAND} -E chdir ${SECP256K1_EMSRC_DIR} emconfigure ./configure
            --enable-module-recovery
            --enable-module-extrakeys
            --disable-tests
            --disable-exhaustive-tests
            --disable-benchmark
            --disable-examples
            --prefix=${SECP256K1_BINARY_DIR}
            --with-pic
            --disable-shared
            --enable-static
          COMMAND ${CMAKE_COMMAND} -E env
            CC=${CMAKE_C_COMPILER}
            CXX=${CMAKE_CXX_COMPILER}
            AR=${SECP256K1_AR}
            RANLIB=${SECP256K1_RANLIB}
            ${CMAKE_COMMAND} -E chdir ${SECP256K1_EMSRC_DIR} emmake make clean
          COMMAND ${CMAKE_COMMAND} -E env
            CC=${CMAKE_C_COMPILER}
            CXX=${CMAKE_CXX_COMPILER}
            AR=${SECP256K1_AR}
            RANLIB=${SECP256K1_RANLIB}
            ${CMAKE_COMMAND} -E chdir ${SECP256K1_EMSRC_DIR} emmake make -j16
          COMMAND ${CMAKE_COMMAND} -E env
            CC=${CMAKE_C_COMPILER}
            CXX=${CMAKE_CXX_COMPILER}
            AR=${SECP256K1_AR}
            RANLIB=${SECP256K1_RANLIB}
            ${CMAKE_COMMAND} -E chdir ${SECP256K1_EMSRC_DIR} emmake make install
          COMMENT "Build Secp256k1 with emscripten"
          DEPENDS ${SECP256K1_SOURCE_DIR}/configure.ac
          OUTPUT ${SECP256K1_LIBRARY}
      )
    elseif (ANDROID)
      set(SECP256K1_BINARY_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/secp256k1/${TON_ANDROID_ARCH_DIR})
      set(SECP256K1_LIBRARY ${SECP256K1_BINARY_DIR}/lib/libsecp256k1.a)
      set(SECP256K1_INCLUDE_DIR ${TON_ANDROID_THIRD_PARTY_DIR}/secp256k1/include)
      set(SECP256K1_LIBRARY ${SECP256K1_LIBRARY} CACHE FILEPATH "Secp256k1 library" FORCE)
      set(SECP256K1_INCLUDE_DIR ${SECP256K1_INCLUDE_DIR} CACHE PATH "Secp256k1 include dir" FORCE)
      set(SECP256K1_AR ${TON_ANDROID_AR})
      set(SECP256K1_RANLIB ${TON_ANDROID_RANLIB})
      if (CMAKE_C_FLAGS)
        set(SECP256K1_CFLAGS "${CMAKE_C_FLAGS} -fPIC")
      else()
        set(SECP256K1_CFLAGS "-fPIC")
      endif()
      file(MAKE_DIRECTORY ${SECP256K1_BINARY_DIR})
      file(MAKE_DIRECTORY ${SECP256K1_INCLUDE_DIR})
      add_custom_command(
          WORKING_DIRECTORY ${SECP256K1_SOURCE_DIR}
          COMMAND ${CMAKE_COMMAND} -E rm -f ${SECP256K1_LIBRARY}
          COMMAND ${CMAKE_COMMAND} -E rm -rf .libs src/.libs config.cache config.status config.log Makefile libtool
          COMMAND ${CMAKE_COMMAND} -E rm -f libsecp256k1.la libsecp256k1_precomputed.la libsecp256k1_common.la libsecp256k1.pc libsecp256k1-config
          COMMAND ${CMAKE_COMMAND} -E env
            CC=${TON_ANDROID_CC}
            CXX=${TON_ANDROID_CXX}
            AR=${SECP256K1_AR}
            RANLIB=${SECP256K1_RANLIB}
            CFLAGS=${SECP256K1_CFLAGS}
            ./autogen.sh
          COMMAND ${CMAKE_COMMAND} -E env
            CC=${TON_ANDROID_CC}
            CXX=${TON_ANDROID_CXX}
            AR=${SECP256K1_AR}
            RANLIB=${SECP256K1_RANLIB}
            CFLAGS=${SECP256K1_CFLAGS}
            ./configure -q --disable-option-checking --enable-module-recovery --enable-module-extrakeys --prefix ${SECP256K1_BINARY_DIR} --with-pic --disable-shared --enable-static --disable-tests --disable-benchmark --host=${TON_ANDROID_HOST}
          COMMAND ${CMAKE_COMMAND} -E env
            CC=${TON_ANDROID_CC}
            CXX=${TON_ANDROID_CXX}
            AR=${SECP256K1_AR}
            RANLIB=${SECP256K1_RANLIB}
            CFLAGS=${SECP256K1_CFLAGS}
            make clean
          COMMAND ${CMAKE_COMMAND} -E env
            CC=${TON_ANDROID_CC}
            CXX=${TON_ANDROID_CXX}
            AR=${SECP256K1_AR}
            RANLIB=${SECP256K1_RANLIB}
            CFLAGS=${SECP256K1_CFLAGS}
            make -j16
          COMMAND ${CMAKE_COMMAND} -E env
            CC=${TON_ANDROID_CC}
            CXX=${TON_ANDROID_CXX}
            AR=${SECP256K1_AR}
            RANLIB=${SECP256K1_RANLIB}
            CFLAGS=${SECP256K1_CFLAGS}
            make install
          COMMAND ${SECP256K1_RANLIB} ${SECP256K1_LIBRARY}
          COMMAND ${CMAKE_COMMAND} -E copy_directory ${SECP256K1_SOURCE_DIR}/include ${SECP256K1_INCLUDE_DIR}
          COMMENT "Build secp256k1 (Android)"
          DEPENDS ${SECP256K1_SOURCE_DIR}
          OUTPUT ${SECP256K1_LIBRARY}
      )
    else()
      if (NOT NIX)
        set(SECP256K1_LIBRARY ${SECP256K1_BINARY_DIR}/lib/libsecp256k1.a)
        if (MINGW)
          set(SECP256K1_AR ar)
          set(SECP256K1_RANLIB ranlib)
          set(MSYS2_BASH "C:/msys64/usr/bin/bash.exe")
        elseif (CMAKE_RANLIB)
          set(SECP256K1_AR ${CMAKE_AR})
          set(SECP256K1_RANLIB ${CMAKE_RANLIB})
        else()
          set(SECP256K1_AR ${CMAKE_AR})
          set(SECP256K1_RANLIB ranlib)
        endif()
        if (MINGW)
          set(SECP256K1_POST_RANLIB ${CMAKE_COMMAND} -E echo "Skip ranlib on MinGW")
        else()
          set(SECP256K1_POST_RANLIB ${SECP256K1_RANLIB} ${SECP256K1_LIBRARY})
        endif()
        if (MINGW)
          add_custom_command(
              WORKING_DIRECTORY ${SECP256K1_SOURCE_DIR}
              COMMAND ${CMAKE_COMMAND} -E rm -f ${SECP256K1_LIBRARY}
              COMMAND ${CMAKE_COMMAND} -E rm -rf .libs src/.libs config.cache config.status config.log Makefile libtool
              COMMAND ${CMAKE_COMMAND} -E rm -f libsecp256k1.la libsecp256k1_precomputed.la libsecp256k1_common.la libsecp256k1.pc libsecp256k1-config
              COMMAND ${MSYS2_BASH} -lc "CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} AR=${SECP256K1_AR} RANLIB=${SECP256K1_RANLIB} ./autogen.sh"
              COMMAND ${MSYS2_BASH} -lc "CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} AR=${SECP256K1_AR} RANLIB=${SECP256K1_RANLIB} ./configure -q --disable-option-checking --enable-module-recovery --enable-module-extrakeys --prefix ${SECP256K1_BINARY_DIR} --with-pic --disable-shared --enable-static --disable-tests --disable-benchmark"
              COMMAND ${MSYS2_BASH} -lc "CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} AR=${SECP256K1_AR} RANLIB=${SECP256K1_RANLIB} make clean"
              COMMAND ${MSYS2_BASH} -lc "CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} AR=${SECP256K1_AR} RANLIB=${SECP256K1_RANLIB} make -j16"
              COMMAND ${MSYS2_BASH} -lc "CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} AR=${SECP256K1_AR} RANLIB=${SECP256K1_RANLIB} make install"
              COMMAND ${SECP256K1_POST_RANLIB}
              COMMENT "Build secp256k1"
              DEPENDS ${SECP256K1_SOURCE_DIR}
              OUTPUT ${SECP256K1_LIBRARY}
          )
        else()
          add_custom_command(
              WORKING_DIRECTORY ${SECP256K1_SOURCE_DIR}
              COMMAND ${CMAKE_COMMAND} -E rm -f ${SECP256K1_LIBRARY}
              COMMAND ${CMAKE_COMMAND} -E rm -rf .libs src/.libs config.cache config.status config.log Makefile libtool
              COMMAND ${CMAKE_COMMAND} -E rm -f libsecp256k1.la libsecp256k1_precomputed.la libsecp256k1_common.la libsecp256k1.pc libsecp256k1-config
              COMMAND ${CMAKE_COMMAND} -E env
                CC=${CMAKE_C_COMPILER}
                CXX=${CMAKE_CXX_COMPILER}
                AR=${SECP256K1_AR}
                RANLIB=${SECP256K1_RANLIB}
                ./autogen.sh
            COMMAND ${CMAKE_COMMAND} -E env
              CC=${CMAKE_C_COMPILER}
              CXX=${CMAKE_CXX_COMPILER}
              AR=${SECP256K1_AR}
              RANLIB=${SECP256K1_RANLIB}
              ./configure -q --disable-option-checking --enable-module-recovery --enable-module-extrakeys --prefix ${SECP256K1_BINARY_DIR} --with-pic --disable-shared --enable-static --disable-tests --disable-benchmark
            COMMAND ${CMAKE_COMMAND} -E env
              CC=${CMAKE_C_COMPILER}
              CXX=${CMAKE_CXX_COMPILER}
              AR=${SECP256K1_AR}
              RANLIB=${SECP256K1_RANLIB}
              make clean
            COMMAND ${CMAKE_COMMAND} -E env
              CC=${CMAKE_C_COMPILER}
              CXX=${CMAKE_CXX_COMPILER}
              AR=${SECP256K1_AR}
              RANLIB=${SECP256K1_RANLIB}
              make -j16
              COMMAND ${CMAKE_COMMAND} -E env
                CC=${CMAKE_C_COMPILER}
                CXX=${CMAKE_CXX_COMPILER}
                AR=${SECP256K1_AR}
                RANLIB=${SECP256K1_RANLIB}
                make install
              COMMAND ${SECP256K1_POST_RANLIB}
              COMMENT "Build secp256k1"
              DEPENDS ${SECP256K1_SOURCE_DIR}
              OUTPUT ${SECP256K1_LIBRARY}
          )
        endif()
      endif()
    endif()
else()
   message(STATUS "Use Secp256k1: ${SECP256K1_LIBRARY}")
endif()

add_custom_target(secp256k1 DEPENDS ${SECP256K1_LIBRARY})
