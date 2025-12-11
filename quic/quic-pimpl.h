#pragma once
#include <openssl/rand.h>
#include <openssl/types.h>

#include "ngtcp2/ngtcp2_crypto.h"
#include "ngtcp2/ngtcp2_crypto_ossl.h"
#include "td/actor/ActorId.h"
#include "td/utils/port/UdpSocketFd.h"

#include "quic-connection.h"

namespace ton::quic {
struct QuicConnectionPImpl {
  class Callback {
   public:
    struct HandshakeCompletedEvent {};
    struct StreamDataEvent {
      td::CSlice data;
      bool fin = false;
    };

    virtual void on_handshake_completed(const HandshakeCompletedEvent& event) = 0;
    virtual void on_stream_data(const StreamDataEvent& event) = 0;

    virtual ~Callback() = default;
  };

  constexpr static size_t QUIC_MTU = 1350;
  constexpr static size_t UDP_MTU = 2048;

  td::UdpSocketFd fd = {};
  td::IPAddress local_address = {};
  td::IPAddress remote_address = {};

  ngtcp2_conn* conn = nullptr;
  SSL_CTX* ssl_ctx = nullptr;
  SSL* ssl = nullptr;

  ngtcp2_crypto_ossl_ctx* ossl_ctx = nullptr;
  ngtcp2_crypto_conn_ref conn_ref{};

  int64_t out_sid = -1;

  std::unique_ptr<Callback> callback = nullptr;

  [[nodiscard]] td::Status init_tls(td::CSlice host, td::CSlice alpn);
  [[nodiscard]] td::Status init_quic();

  [[nodiscard]] td::Status flush_egress();
  [[nodiscard]] td::Status handle_ingress();

  [[nodiscard]] td::Status write_stream(td::CSlice data, bool fin);

  ~QuicConnectionPImpl() {
    if (conn) {
      ngtcp2_conn_del(conn);
      conn = nullptr;
    }
    if (ossl_ctx) {
      ngtcp2_crypto_ossl_ctx_del(ossl_ctx);
      ossl_ctx = nullptr;
    }
    if (ssl) {
      SSL_set_app_data(ssl, NULL);
      SSL_free(ssl);
      ssl = nullptr;
    }
    if (ssl_ctx) {
      SSL_CTX_free(ssl_ctx);
      ssl_ctx = nullptr;
    }
    if (!fd.empty()) {
      fd.close();
    }
  }

 private:
  static ngtcp2_tstamp now_ts();

  static ngtcp2_conn* get_pimpl_from_ref(ngtcp2_crypto_conn_ref* ref);

  static void rand_cb(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx);
  static int get_new_connection_id_cb(ngtcp2_conn* conn, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                      void* user_data);
  static int handshake_completed_cb(ngtcp2_conn* conn, void* user_data);
  static int recv_stream_data_cb(ngtcp2_conn* /*conn*/, uint32_t flags, int64_t stream_id, uint64_t offset,
                                 const uint8_t* data, size_t datalen, void* user_data, void* stream_user_data);
};
}  // namespace ton::quic