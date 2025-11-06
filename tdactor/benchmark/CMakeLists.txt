set(BENCHMARK_SOURCE
  benchmark.cpp
  third_party/mp-queue.c

  third_party/FAAArrayQueue.h
  third_party/LCRQueue.h
  third_party/LazyIndexArrayQueue.h
  third_party/MoodyCamelQueue.h
)
add_executable(benchmark ${BENCHMARK_SOURCE})
target_include_directories(benchmark PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(benchmark PRIVATE tdactor)

if (MSVC)
  set_property(SOURCE benchmark.cpp APPEND_STRING PROPERTY COMPILE_FLAGS " /wd4457 /wd4316")
endif()

add_executable(benchmark-coro benchmark-coro.cpp)
target_link_libraries(benchmark-coro tdactor tdutils)

# Google Benchmark based coroutine benchmarks
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(GBENCH benchmark)
  if(GBENCH_FOUND)
    add_executable(gbench-coro gbench-coro.cpp)
    target_link_libraries(gbench-coro tdactor tdutils)
    # Use full library paths to avoid conflicts with our "benchmark" executable
    target_link_directories(gbench-coro PRIVATE ${GBENCH_LIBRARY_DIRS})
    target_link_libraries(gbench-coro -lbenchmark)
    target_include_directories(gbench-coro PRIVATE ${GBENCH_INCLUDE_DIRS})
    target_compile_options(gbench-coro PRIVATE ${GBENCH_CFLAGS_OTHER})
    
    message(STATUS "Google Benchmark found via pkg-config - gbench-coro target enabled")
    
    # Check for Folly
    find_package(folly CONFIG QUIET)
    if(folly_FOUND)
      add_executable(gbench-coro-folly gbench-coro-folly.cpp)
      target_link_libraries(gbench-coro-folly -lbenchmark)
      target_link_directories(gbench-coro-folly PRIVATE ${GBENCH_LIBRARY_DIRS})
      target_include_directories(gbench-coro-folly PRIVATE ${GBENCH_INCLUDE_DIRS})
      target_compile_options(gbench-coro-folly PRIVATE ${GBENCH_CFLAGS_OTHER})

      find_package(gflags REQUIRED)
      target_link_libraries(gbench-coro-folly Folly::folly gflags)

      message(STATUS "Folly found - enabling Folly benchmarks in gbench-coro-folly")
    else()
      message(STATUS "Folly not found - Folly benchmarks disabled")
    endif()

    add_executable(gbench-coro-yaclib gbench-coro-yaclib.cpp)
    target_link_libraries(gbench-coro-yaclib -lbenchmark)
    target_link_directories(gbench-coro-yaclib PRIVATE ${GBENCH_LIBRARY_DIRS})
    target_include_directories(gbench-coro-yaclib PRIVATE ${GBENCH_INCLUDE_DIRS})
    target_compile_options(gbench-coro-yaclib PRIVATE ${GBENCH_CFLAGS_OTHER})

    # Check for YACLib
    find_package(yaclib CONFIG QUIET)
    if(yaclib_FOUND)
      target_link_libraries(gbench-coro-yaclib yaclib::yaclib)
      message(STATUS "YACLib found - enabling YACLib benchmarks in gbench-coro-yaclib")
    else()
      message(STATUS "YACLib not found - fetching via FetchContent for gbench-coro-yaclib")
      include(FetchContent)
      set(YACLIB_FLAGS "CORO" CACHE STRING "" FORCE)
      FetchContent_Declare(yaclib
        GIT_REPOSITORY https://github.com/YACLib/YACLib.git
        GIT_TAG main
      )
      FetchContent_MakeAvailable(yaclib)
      target_link_libraries(gbench-coro-yaclib yaclib)
    endif()
    target_compile_definitions(gbench-coro-yaclib PRIVATE YACLIB_CORO=2)

  else()
    message(STATUS "Google Benchmark not found via pkg-config - gbench-coro target disabled")
  endif()
else()
  message(STATUS "PkgConfig not found - gbench-coro target disabled")
endif()