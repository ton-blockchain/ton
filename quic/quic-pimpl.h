#pragma once
#include <openssl/rand.h>
#include <openssl/types.h>
#include <string>

#include "ngtcp2/ngtcp2.h"
#include "ngtcp2/ngtcp2_crypto.h"
#include "ngtcp2/ngtcp2_crypto_ossl.h"
#include "td/actor/ActorId.h"
#include "td/utils/port/UdpSocketFd.h"

#include "quic-client.h"
#include "quic-common.h"

namespace ton::quic {
struct QuicConnectionPImpl {
  class Callback {
   public:
    struct HandshakeCompletedEvent {};
    struct StreamDataEvent {
      QuicStreamID sid = 0;
      td::BufferSlice data;
      bool fin = false;
    };

    virtual void on_handshake_completed(HandshakeCompletedEvent event) = 0;
    virtual void on_stream_data(StreamDataEvent event) = 0;

    virtual ~Callback() = default;
  };

  constexpr static size_t DEFAULT_WINDOW = 1 << 20;
  constexpr static size_t DEFAULT_STREAM_LIMIT = 64;

  td::IPAddress local_address = {};
  td::IPAddress remote_address = {};

  ngtcp2_conn* conn = nullptr;
  SSL_CTX* ssl_ctx = nullptr;
  SSL* ssl = nullptr;

  ngtcp2_crypto_ossl_ctx* ossl_ctx = nullptr;
  ngtcp2_crypto_conn_ref conn_ref{};

  std::unique_ptr<Callback> callback = nullptr;

  [[nodiscard]] td::Status init_tls_client(td::Slice host, td::Slice alpn);
  [[nodiscard]] td::Status init_tls_server(td::Slice cert_file, td::Slice key_file, td::Slice alpn);

  [[nodiscard]] td::Status init_quic_client();
  [[nodiscard]] td::Status init_quic_server(const ngtcp2_version_cid& vc);

  [[nodiscard]] td::Status produce_egress(UdpMessageBuffer& msg_out);
  [[nodiscard]] td::Status handle_ingress(const UdpMessageBuffer& msg_in);

  [[nodiscard]] td::Result<QuicStreamID> open_stream();
  [[nodiscard]] td::Status write_stream(UdpMessageBuffer& msg_out, QuicStreamID sid, td::Slice data, bool fin);

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
  }

 private:
  std::string alpn_wire_;

  static ngtcp2_tstamp now_ts();

  static ngtcp2_conn* get_pimpl_from_ref(ngtcp2_crypto_conn_ref* ref);

  static void rand_cb(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx);
  static int get_new_connection_id_cb(ngtcp2_conn* conn, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                      void* user_data);
  static int handshake_completed_cb(ngtcp2_conn* conn, void* user_data);
  static int recv_stream_data_cb(ngtcp2_conn* /*conn*/, uint32_t flags, int64_t stream_id, uint64_t offset,
                                 const uint8_t* data, size_t datalen, void* user_data, void* stream_user_data);

  static int alpn_select_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                            unsigned int inlen, void* arg);
};
}  // namespace ton::quic