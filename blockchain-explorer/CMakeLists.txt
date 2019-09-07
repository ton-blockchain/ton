cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)


find_package(MHD)

if (MHD_FOUND)

  set(BLOCHAIN_EXPLORER_SOURCE 
    blockchain-explorer.cpp
    blockchain-explorer.hpp
    blockchain-explorer-http.cpp
    blockchain-explorer-http.hpp
    blockchain-explorer-query.cpp
    blockchain-explorer-query.hpp
    )

  add_executable(blockchain-explorer ${BLOCHAIN_EXPLORER_SOURCE})
  target_include_directories(blockchain-explorer PUBLIC ${MHD_INCLUDE_DIRS})
  target_link_libraries(blockchain-explorer tdutils tdactor adnllite tl_lite_api tl-lite-utils
    ton_crypto ton_block ${MHD_LIBRARY})

endif()
