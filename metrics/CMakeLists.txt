add_library(metrics STATIC prometheus-exporter.cpp metrics-types.cpp metrics-collectors.cpp)
target_include_directories(metrics PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/..)
target_link_libraries(metrics PUBLIC tdnet tonlib tonhttp tdutils tdactor)

add_executable(metrics-example metrics-example.cpp)
target_include_directories(metrics-example PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(metrics-example PUBLIC metrics)
