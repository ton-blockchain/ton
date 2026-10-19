# Manual, all-in-memory benchmark for the real collator and validator. Fixture
# construction is intentionally expensive, so this target is excluded from
# normal builds and is not registered with CTest.
add_executable(collation-validation-bench EXCLUDE_FROM_ALL
  bench.cpp
  corpus.cpp
  corpus.h
  fixture.cpp
  fixture.h
  ../common.cpp
  ../common.h
  ../../validator/downloaders/wait-block-data.cpp
)
target_include_directories(collation-validation-bench PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/benchmark
)
target_compile_definitions(collation-validation-bench PRIVATE TON_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(collation-validation-bench PRIVATE
  tdactor
  tdutils
  ton_crypto
  ton_block
  ton_validator
  validator-hardfork
  validator-common
  validator-db
  ${JEMALLOC_LIBRARIES}
)
if (TON_USE_JEMALLOC)
  target_compile_definitions(collation-validation-bench PRIVATE TON_USE_JEMALLOC=1)
endif()
