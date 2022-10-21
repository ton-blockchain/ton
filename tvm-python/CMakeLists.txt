cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

find_package(Python REQUIRED Development)

set(PYTHON_TVM_SOURCE tvm_python.cpp)
set(PYTHON_TVM_VERSION 0.33)

file(COPY ../crypto/fift/lib DESTINATION ./)

pybind11_add_module(tvm_python ${PYTHON_TVM_SOURCE})
target_link_libraries(tvm_python PRIVATE overlay tdutils tdactor adnl tl_api dht
        rldp catchain validatorsession full-node validator-disk validator-hardfork ton_validator
        validator-hardfork fift-lib memprof ${JEMALLOC_LIBRARIES})
