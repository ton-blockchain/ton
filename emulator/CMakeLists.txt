option(EMULATOR_STATIC "Build emulator as static library" OFF)

set(EMULATOR_STATIC_SOURCE
  transaction-emulator.cpp
  tvm-emulator.hpp
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

if (EMULATOR_STATIC OR USE_EMSCRIPTEN)
  add_library(emulator STATIC ${EMULATOR_SOURCE})
else()
  add_library(emulator SHARED ${EMULATOR_SOURCE})
endif()

if (PORTABLE AND NOT APPLE)
  target_link_libraries(emulator PUBLIC emulator_static git -static-libgcc -static-libstdc++)
else()
  target_link_libraries(emulator PUBLIC emulator_static git)
endif()

if (APPLE)
  set(CMAKE_MACOSX_RPATH ON)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

if (APPLE AND PORTABLE)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libstdc++")
endif()

generate_export_header(emulator EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/emulator_export.h)
if (EMULATOR_STATIC OR USE_EMSCRIPTEN)
  target_compile_definitions(emulator PUBLIC EMULATOR_STATIC_DEFINE)
endif()
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
