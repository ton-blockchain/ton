set(TDFEC_SOURCE
  td/fec/raptorq/Rfc.cpp
  td/fec/raptorq/Rfc.h
  td/fec/raptorq/Decoder.h
  td/fec/raptorq/Decoder.cpp
  td/fec/raptorq/Encoder.h
  td/fec/raptorq/Encoder.cpp
  td/fec/raptorq/RawEncoder.cpp
  td/fec/raptorq/RawEncoder.h
  td/fec/raptorq/Solver.h
  td/fec/raptorq/Solver.cpp

  td/fec/common/SymbolRef.h
  td/fec/common/SymbolsView.h
  td/fec/common/SymbolsView.cpp

  td/fec/online/Encoder.h
  td/fec/online/Encoder.cpp
  td/fec/online/Decoder.h
  td/fec/online/Decoder.cpp
  td/fec/online/Rfc.h
  td/fec/online/Rfc.cpp

  td/fec/algebra/BeliefPropagationDecoding.h
  td/fec/algebra/BeliefPropagationDecoding.cpp
  td/fec/algebra/GaussianElimination.h
  td/fec/algebra/GaussianElimination.cpp
  td/fec/algebra/InactivationDecoding.h
  td/fec/algebra/InactivationDecoding.cpp
  td/fec/algebra/SparseMatrixGF2.h
  td/fec/algebra/SparseMatrixGF2.cpp
  td/fec/algebra/MatrixGF2.h
  td/fec/algebra/MatrixGF2.cpp
  td/fec/algebra/MatrixGF256.h
  td/fec/algebra/MatrixGF256.cpp
  td/fec/algebra/Octet.h
  td/fec/algebra/Octet.cpp
  td/fec/algebra/Simd.h

  td/fec/fec.cpp
  td/fec/fec.h
)

add_library(tdfec STATIC ${TDFEC_SOURCE})

target_include_directories(tdfec PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/..
)
target_link_libraries(tdfec PUBLIC tdutils)

if (USE_LIBRAPTORQ)
  set(THIRD_PARTY_FEC_SOURCE
    test/LibRaptorQ.h
    test/LibRaptorQ.cpp
  )

  add_library(third_party_fec STATIC ${THIRD_PARTY_FEC_SOURCE})
  target_include_directories(third_party_fec PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/..
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/../third-party/libraptorq/src
  )

  target_link_libraries(third_party_fec PRIVATE tdutils RaptorQ_Static)
endif()


set(FEC_TEST_SOURCE
  ${CMAKE_CURRENT_SOURCE_DIR}/test/fec-test.cpp
  PARENT_SCOPE
)

add_subdirectory(benchmark)
