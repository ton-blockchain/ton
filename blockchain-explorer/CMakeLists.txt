option(NIX "Use \"ON\" for a static build." OFF)

set(BLOCHAIN_EXPLORER_SOURCE
    blockchain-explorer.cpp
    blockchain-explorer.hpp
    blockchain-explorer-http.cpp
    blockchain-explorer-http.hpp
    blockchain-explorer-query.cpp
    blockchain-explorer-query.hpp
)

add_executable(blockchain-explorer ${BLOCHAIN_EXPLORER_SOURCE})

if (NIX)
  if (MHD_FOUND)
    target_include_directories(blockchain-explorer PUBLIC ${MHD_INCLUDE_DIR})
    target_link_libraries(blockchain-explorer tdactor adnllite tl_lite_api tl-lite-utils ton_crypto ${MHD_LIBRARY})
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MHD libmicrohttpd)
    target_include_directories(blockchain-explorer PUBLIC ${MHD_INCLUDE_DIR} ${MHD_STATIC_INCLUDE_DIRS})
    target_link_libraries(blockchain-explorer tdactor adnllite tl_lite_api tl-lite-utils ton_crypto ${MHD_LIBRARIES} ${MHD_STATIC_LIBRARIES})
  endif()
else()
  if (MHD_FOUND)
    target_include_directories(blockchain-explorer PUBLIC ${MHD_INCLUDE_DIR})
    target_link_libraries(blockchain-explorer tdactor adnllite tl_lite_api tl-lite-utils ton_crypto ${MHD_LIBRARY})
  else()
    find_package(MHD)
    target_include_directories(blockchain-explorer PUBLIC ${MHD_INCLUDE_DIR})
    target_link_libraries(blockchain-explorer tdactor adnllite tl_lite_api tl-lite-utils ton_crypto ${MHD_LIBRARY})
  endif()
endif()

target_include_directories(blockchain-explorer PUBLIC ${MHD_INCLUDE_DIR})
target_link_libraries(blockchain-explorer tdactor adnllite tl_lite_api tl-lite-utils ton_crypto ${MHD_LIBRARY})
target_link_libraries(blockchain-explorer lite-client-common)

install(TARGETS blockchain-explorer RUNTIME DESTINATION bin)

