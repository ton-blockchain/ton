# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.4.1 FATAL_ERROR)

option(TONLIB_ENABLE_JNI "Enable JNI-compatible TonLib API" ON)

if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  string(APPEND CMAKE_CXX_FLAGS " -std=c++14 -fno-omit-frame-pointer -ffunction-sections -fdata-sections")
else()
  string(APPEND CMAKE_CXX_FLAGS " -std=c++14 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wconversion -Wno-sign-conversion -fno-omit-frame-pointer -ffunction-sections -fdata-sections")
endif()
if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--gc-sections -Wl,--exclude-libs,ALL")
endif()

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.

add_library( # Sets the name of the library.
             native-lib

             # Sets the library as a shared library.
             SHARED

             # Provides a relative path to your source file(s).
             native-lib.cpp)


list(APPEND CMAKE_FIND_ROOT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crypto/${ANDROID_ARCH_NAME}")
set(TON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${TON_DIR} ton EXCLUDE_FROM_ALL)
target_link_libraries(native-lib tonlibjson_static)
target_link_libraries(native-lib tonlib)

#target_sources(native-lib PRIVATE ${ALL_TEST_SOURCE})
#target_link_libraries(native-lib all_tests)

set(TONLIB_API_JAVA_PACKAGE "drinkless/org/ton")
target_compile_definitions(native-lib PRIVATE PACKAGE_NAME="${TONLIB_API_JAVA_PACKAGE}")

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  add_custom_command(TARGET native-lib POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E rename $<TARGET_FILE:native-lib> $<TARGET_FILE:native-lib>.debug
          COMMAND ${CMAKE_STRIP} -S $<TARGET_FILE:native-lib>.debug -o $<TARGET_FILE:native-lib>)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  add_custom_command(TARGET native-lib POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E rename $<TARGET_FILE:native-lib> $<TARGET_FILE:native-lib>.debug
          COMMAND ${CMAKE_STRIP} --strip-debug --strip-unneeded $<TARGET_FILE:native-lib>.debug -o $<TARGET_FILE:native-lib>)
endif()

if (NOT CMAKE_CROSSCOMPILING)
  set(TONLIB_API_JAVA_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/)
  file(MAKE_DIRECTORY ${TONLIB_API_JAVA_PATH}${TONLIB_API_JAVA_PACKAGE})
  set(TONLIB_API_TLO_PATH ${TON_DIR}/tl/generate/scheme/tonlib_api.tlo)
  set(TONLIB_API_TL_PATH ${TON_DIR}/tl/generate/scheme/tonlib_api.tl)
  set(JAVADOC_TL_DOCUMENTATION_GENERATOR_PATH ${TON_DIR}/tl/generate/JavadocTlDocumentationGenerator.php)
  set(GENERATE_JAVA_CMD tonlib_generate_java_api TonApi ${TONLIB_API_TLO_PATH} ${TONLIB_API_JAVA_PATH} ${TONLIB_API_JAVA_PACKAGE})
  if (PHP_EXECUTABLE)
    set(GENERATE_JAVA_CMD ${GENERATE_JAVA_CMD} && ${PHP_EXECUTABLE} ${JAVADOC_TL_DOCUMENTATION_GENERATOR_PATH}
        ${TONLIB_API_TL_PATH} ${TONLIB_API_JAVA_PATH}/${TONLIB_API_JAVA_PACKAGE}/TonApi.java androidx.annotation.Nullable @Nullable)
  endif()

  add_custom_target(tl_generate_java
    COMMAND ${GENERATE_JAVA_CMD}
    COMMENT "Generate java tl source files"
    DEPENDS tonlib_generate_java_api ${TONLIB_API_TLO_PATH}
  )
  add_dependencies(prepare_cross_compiling tl_generate_java)
endif()
