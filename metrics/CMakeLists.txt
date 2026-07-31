add_library(metrics STATIC prometheus-exporter.cpp types.cpp tl-traffic-bucket.cpp)
target_include_directories(metrics PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/..)
target_link_libraries(metrics PUBLIC tdnet tonlib tonhttp tdutils tdactor)
target_link_libraries(metrics PRIVATE tl_api tl_lite_api)
