# Script to patch LZ4 CMakeLists.txt to update cmake_minimum_required version
# This avoids shell escaping issues with sed on different platforms

if(NOT DEFINED INPUT_FILE)
    message(FATAL_ERROR "INPUT_FILE must be defined")
endif()

if(NOT EXISTS "${INPUT_FILE}")
    message(FATAL_ERROR "Input file does not exist: ${INPUT_FILE}")
endif()

# Read the file
file(READ "${INPUT_FILE}" FILE_CONTENTS)

# Replace cmake_minimum_required(VERSION 2.8.12) with cmake_minimum_required(VERSION 3.5)
string(REPLACE "cmake_minimum_required(VERSION 2.8.12)" "cmake_minimum_required(VERSION 3.5)" FILE_CONTENTS "${FILE_CONTENTS}")

# Write the file back
file(WRITE "${INPUT_FILE}" "${FILE_CONTENTS}")

message(STATUS "Successfully patched ${INPUT_FILE}")
