cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(EMULATOR_STATIC_SOURCE
  transaction-emulator.cpp
  tvm-emulator.hpp
)

set(EMULATOR_HEADERS 
  transaction-emulator.h
  emulator-extern.h
)

set(EMULATOR_SOURCE
  emulator-extern.cpp
)

set(EMULATOR_EMSCRIPTEN_SOURCE
  transaction-emscripten.cpp
)

include(GenerateExportHeader)

add_library(emulator_static STATIC ${EMULATOR_STATIC_SOURCE})
target_link_libraries(emulator_static PUBLIC ton_crypto ton_block smc-envelope)

add_library(emulator SHARED ${EMULATOR_SOURCE} ${EMULATOR_HEADERS})
target_link_libraries(emulator PUBLIC emulator_static)
generate_export_header(emulator EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/emulator_export.h)
target_include_directories(emulator PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
if (APPLE)
  set_target_properties(emulator PROPERTIES LINK_FLAGS "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/emulator_export_list")
endif()

if (USE_EMSCRIPTEN)
  add_executable(emulator-emscripten ${EMULATOR_EMSCRIPTEN_SOURCE})
  target_link_libraries(emulator-emscripten PUBLIC emulator)
  target_link_options(emulator-emscripten PRIVATE -sEXPORTED_RUNTIME_METHODS=_malloc,free,UTF8ToString,stringToUTF8,allocate,ALLOC_NORMAL,lengthBytesUTF8)
  target_link_options(emulator-emscripten PRIVATE -sEXPORTED_FUNCTIONS=_emulate,_free,_run_get_method)
  target_link_options(emulator-emscripten PRIVATE -sEXPORT_NAME=EmulatorModule)
  target_link_options(emulator-emscripten PRIVATE -sERROR_ON_UNDEFINED_SYMBOLS=0)
  target_link_options(emulator-emscripten PRIVATE -Oz)
  target_link_options(emulator-emscripten PRIVATE -sIGNORE_MISSING_MAIN=1)
  target_link_options(emulator-emscripten PRIVATE -sAUTO_NATIVE_LIBRARIES=0)
  target_link_options(emulator-emscripten PRIVATE -sMODULARIZE=1)
  target_link_options(emulator-emscripten PRIVATE -sENVIRONMENT=web)
  target_link_options(emulator-emscripten PRIVATE -sFILESYSTEM=0)
  target_link_options(emulator-emscripten PRIVATE -fexceptions)
  target_compile_options(emulator-emscripten PRIVATE -fexceptions)
endif()