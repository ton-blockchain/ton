set(TOLK_SOURCE
        src-file.cpp
        lexer.cpp
        symtable.cpp
        compiler-state.cpp
        ast.cpp
        ast-from-tokens.cpp
        constant-evaluator.cpp
        pack-unpack-api.cpp
        pack-unpack-serializers.cpp
        pipe-discover-parse-sources.cpp
        pipe-register-symbols.cpp
        pipe-resolve-identifiers.cpp
        pipe-resolve-types.cpp
        pipe-calc-rvalue-lvalue.cpp
        pipe-infer-types-and-calls.cpp
        pipe-check-inferred-types.cpp
        pipe-refine-lvalue-for-mutate.cpp
        pipe-check-rvalue-lvalue.cpp
        pipe-check-pure-impure.cpp
        pipe-check-serialized-fields.cpp
        pipe-constant-folding.cpp
        pipe-optimize-boolean-expr.cpp
        pipe-ast-to-legacy.cpp
        pipe-find-unused-symbols.cpp
        pipe-generate-fif-output.cpp
        type-system.cpp
        smart-casts-cfg.cpp
        generics-helpers.cpp
        abscode.cpp
        analyzer.cpp
        asmops.cpp
        builtins.cpp
        stack-transform.cpp
        optimize.cpp
        codegen.cpp
        tolk.cpp
)

add_executable(tolk tolk-main.cpp ${TOLK_SOURCE})
target_include_directories(tolk PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(tolk PUBLIC git ton_crypto_core)
if (WINGETOPT_FOUND)
  target_link_libraries_system(tolk wingetopt)
endif ()
if (${TOLK_DEBUG}) # -DTOLK_DEBUG=1 in CMake options => #define TOLK_DEBUG (for development purposes)
  message(STATUS "TOLK_DEBUG is ON")
  target_compile_definitions(tolk PRIVATE TOLK_DEBUG=1)
endif()

if (USE_EMSCRIPTEN)
  add_executable(tolkfiftlib tolk-wasm.cpp ${TOLK_SOURCE})
  target_include_directories(tolkfiftlib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
  target_link_libraries(tolkfiftlib PUBLIC fift-lib git)
  target_link_options(tolkfiftlib PRIVATE
          -sEXPORTED_RUNTIME_METHODS=FS,ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,addFunction,removeFunction,setValue
          -sEXPORTED_FUNCTIONS=_tolk_compile,_version,_malloc,_free,_setThrew
          -sEXPORT_NAME=CompilerModule
          -sERROR_ON_UNDEFINED_SYMBOLS=0
          -sFILESYSTEM=1 -lnodefs.js
          -Oz
          -sIGNORE_MISSING_MAIN=1
          -sAUTO_NATIVE_LIBRARIES=0
          -sMODULARIZE=1
          -sTOTAL_MEMORY=33554432
          -sALLOW_MEMORY_GROWTH=1
          -sALLOW_TABLE_GROWTH=1
          --embed-file ${CMAKE_CURRENT_SOURCE_DIR}/../crypto/fift/lib@/fiftlib
          -fexceptions
  )
  target_compile_options(tolkfiftlib PRIVATE -fexceptions -fno-stack-protector)
endif ()

install(TARGETS tolk RUNTIME DESTINATION bin)
