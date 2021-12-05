cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

if (NOT GSL_FOUND)
  find_package(GSL)
endif()

set(RLDP_SOURCE
  Ack.cpp
  Bbr.cpp
  BdwStats.cpp
  FecHelper.cpp
  InboundTransfer.cpp
  LossSender.cpp
  LossStats.cpp
  OutboundTransfer.cpp
  Pacer.cpp
  rldp.cpp
  RldpReceiver.cpp
  RldpSender.cpp
  RldpConnection.cpp
  RttStats.cpp
  SenderPackets.cpp

  Ack.h
  Bbr.h
  BdwStats.h
  FecHelper.h
  InboundTransfer.h
  LossSender.h
  LossStats.h
  OutboundTransfer.h
  Pacer.h
  rldp.h
  rldp.hpp
  RldpReceiver.h
  RldpSender.h
  RldpConnection.h
  RttStats.h
  SenderPackets.h
)

add_library(rldp2 STATIC ${RLDP_SOURCE})

target_include_directories(rldp PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/.. 
  ${OPENSSL_INCLUDE_DIR}
)
if (GSL_FOUND)
  target_link_libraries(rldp2 PRIVATE gsl)
  target_compile_definitions(rldp2 PRIVATE -DTON_HAVE_GSL=1)
endif()
target_link_libraries(rldp2 PUBLIC tdutils tdactor fec adnl tl_api)

