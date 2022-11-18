cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

add_executable(embed-provider-code smartcont/embed-provider-code.cpp)

add_custom_command(
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMAND embed-provider-code smartcont/storage-provider-code.boc smartcont/provider-code.h
  COMMENT "Generate provider-code.h"
  DEPENDS embed-provider-code smartcont/storage-provider-code.boc
  OUTPUT smartcont/provider-code.h
)

set(STORAGE_DAEMON_SOURCE
  storage-daemon.cpp
  StorageManager.h
  StorageManager.cpp
  StorageProvider.h
  StorageProvider.cpp
  smc-util.h
  smc-util.cpp
  smartcont/provider-code.h)

set(STORAGE_DAEMON_CLI_SOURCE
  storage-daemon-cli.cpp
  )

add_executable(storage-daemon ${STORAGE_DAEMON_SOURCE})
target_link_libraries(storage-daemon storage overlay tdutils tdactor adnl tl_api dht
  rldp rldp2 fift-lib memprof git tonlib)

add_executable(storage-daemon-cli ${STORAGE_DAEMON_CLI_SOURCE})
target_link_libraries(storage-daemon-cli tdutils tdactor adnllite tl_api tl_lite_api tl-lite-utils ton_block terminal git)