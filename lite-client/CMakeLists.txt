add_library(lite-client-common STATIC lite-client-common.cpp lite-client-common.h ext-client.cpp ext-client.h
  query-utils.hpp query-utils.cpp)
target_link_libraries(lite-client-common PUBLIC tdactor adnllite tl_api tl_lite_api tl-lite-utils ton_crypto)

add_executable(lite-client lite-client.cpp lite-client.h ext-client.h ext-client.cpp)
target_link_libraries(lite-client tdutils tdactor adnllite tl_api tl_lite_api tl-lite-utils terminal lite-client-common git)

install(TARGETS lite-client RUNTIME DESTINATION bin)
