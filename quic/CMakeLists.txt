add_library(quic STATIC quic-connection.cpp quic-pimpl.cpp)
target_include_directories(quic PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/..
    ${NGTCP2_ROOT}/lib/includes
    ${NGTCP2_ROOT}/crypto/includes
    ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(quic PUBLIC tdutils tdactor ngtcp2_static ngtcp2_crypto_ossl_static OpenSSL::SSL OpenSSL::Crypto)

add_executable(quic-example quic-example.cpp)
target_include_directories(quic-example PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(quic-example PUBLIC quic)
