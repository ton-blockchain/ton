cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

set(STORAGE_DAEMON_SOURCE
  storage-daemon.cpp
  StorageManager.h
  StorageManager.cpp)

set(STORAGE_DAEMON_CLI_SOURCE
  storage-daemon-cli.cpp
)

add_executable(storage-daemon ${STORAGE_DAEMON_SOURCE})
target_link_libraries(storage-daemon storage overlay tdutils tdactor adnl tl_api dht
  rldp rldp2 fift-lib memprof git)

add_executable(storage-daemon-cli ${STORAGE_DAEMON_CLI_SOURCE})
target_link_libraries(storage-daemon-cli tdutils tdactor adnllite tl_api tl_lite_api tl-lite-utils terminal git)