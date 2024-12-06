cmake_minimum_required(VERSION 3.5 FATAL_ERROR)

option(BUILD_SHARED_LIBS "Use \"OFF\" for a static build." ON)

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
  emulator-emscripten.cpp
)

include(GenerateExportHeader)

add_library(emulator_static STATIC ${EMULATOR_STATIC_SOURCE})
target_link_libraries(emulator_static PUBLIC ton_crypto smc-envelope)

if (NOT USE_EMSCRIPTEN AND BUILD_SHARED_LIBS)
  add_library(emulator SHARED ${EMULATOR_SOURCE} ${EMULATOR_HEADERS})
else()
  add_library(emulator STATIC ${EMULATOR_SOURCE} ${EMULATOR_HEADERS})
endif()

target_link_libraries(emulator PUBLIC emulator_static git)
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
  target_link_options(emulator-emscripten PRIVATE -sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,allocate,ALLOC_NORMAL,lengthBytesUTF8)
  target_link_options(emulator-emscripten PRIVATE -sEXPORTED_FUNCTIONS=_emulate,_free,_malloc,_run_get_method,_create_emulator,_destroy_emulator,_emulate_with_emulator,_version)
  target_link_options(emulator-emscripten PRIVATE -sEXPORT_NAME=EmulatorModule)
  target_link_options(emulator-emscripten PRIVATE -sERROR_ON_UNDEFINED_SYMBOLS=0)
  target_link_options(emulator-emscripten PRIVATE -Oz)
  target_link_options(emulator-emscripten PRIVATE -sIGNORE_MISSING_MAIN=1)
  target_link_options(emulator-emscripten PRIVATE -sAUTO_NATIVE_LIBRARIES=0)
  target_link_options(emulator-emscripten PRIVATE -sMODULARIZE=1)
  target_link_options(emulator-emscripten PRIVATE -sENVIRONMENT=web)
  target_link_options(emulator-emscripten PRIVATE -sFILESYSTEM=0)
  target_link_options(emulator-emscripten PRIVATE -sALLOW_MEMORY_GROWTH=1)
  target_link_options(emulator-emscripten PRIVATE -fexceptions)
  if (USE_EMSCRIPTEN_NO_WASM)
    target_link_options(emulator-emscripten PRIVATE -sWASM=0)
  endif()
  target_compile_options(emulator-emscripten PRIVATE -fexceptions)
endif()

install(TARGETS emulator ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
