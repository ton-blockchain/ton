#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "crypto/Ed25519.h"
#include "ngtcp2/ngtcp2.h"
#include "ngtcp2/ngtcp2_crypto.h"
#include "ngtcp2/ngtcp2_crypto_ossl.h"
#include "td/actor/ActorId.h"
#include "td/utils/Time.h"
#include "td/utils/crypto.h"
#include "td/utils/port/UdpSocketFd.h"

#include "openssl-utils.h"
#include "quic-common.h"

namespace ton::quic {
struct QuicConnectionPImpl {
  enum class ExpiryAction {
    None,
    ScheduleWrite,
    IdleClose,
    Close,
  };
  class Callback {
   public:
    struct HandshakeCompletedEvent {
      td::SecureString peer_public_key;  // Ed25519 public key (32 bytes), empty if not available
    };
    struct StreamDataEvent {
      QuicStreamID sid = 0;
      td::BufferSlice data;
      bool fin = false;
    };

    virtual void set_connection_id(QuicConnectionId cid) = 0;
    virtual td::Status on_handshake_completed(HandshakeCompletedEvent event) = 0;
    virtual void on_stream_data(StreamDataEvent event) = 0;

    virtual ~Callback() = default;
  };

  constexpr static size_t DEFAULT_WINDOW = 1 << 20;
  constexpr static size_t CID_LENGTH = 16;
  constexpr static size_t DEFAULT_STREAM_LIMIT = 1ULL << 60ULL;

  struct PrivateTag {};

  QuicConnectionPImpl(PrivateTag, const td::IPAddress& local_address, const td::IPAddress& remote_address,
                      bool is_server, std::unique_ptr<Callback> callback)
      : local_address_(local_address)
      , remote_address_(remote_address)
      , is_server_(is_server)
      , callback_(std::move(callback)) {
  }

  [[nodiscard]] static td::Result<std::unique_ptr<QuicConnectionPImpl>> create_client(
      const td::IPAddress& local_address, const td::IPAddress& remote_address,
      const td::Ed25519::PrivateKey& client_key, td::Slice alpn, std::unique_ptr<Callback> callback);

  [[nodiscard]] static td::Result<std::unique_ptr<QuicConnectionPImpl>> create_server(
      const td::IPAddress& local_address, const td::IPAddress& remote_address,
      const td::Ed25519::PrivateKey& server_key, td::Slice alpn, const ngtcp2_version_cid& vc,
      std::unique_ptr<Callback> callback);

  [[nodiscard]] td::Status produce_egress(UdpMessageBuffer& msg_out);
  [[nodiscard]] td::Status handle_ingress(const UdpMessageBuffer& msg_in);

  [[nodiscard]] td::Timestamp get_expiry_timestamp() const;
  void relax_alarm_timestamp(td::Timestamp& alarm_ts) const {
    alarm_ts.relax(get_expiry_timestamp());
  }
  [[nodiscard]] bool is_expired() const;
  [[nodiscard]] td::Result<ExpiryAction> handle_expiry();

  [[nodiscard]] QuicConnectionId get_primary_scid() const;

  [[nodiscard]] td::Result<QuicStreamID> open_stream();
  [[nodiscard]] td::Status write_stream(UdpMessageBuffer& msg_out, QuicStreamID sid, td::BufferSlice data, bool fin);

  ~QuicConnectionPImpl() {
    if (ssl_) {
      SSL_set_app_data(ssl_.get(), nullptr);
    }
  }

 private:
  td::IPAddress local_address_;
  td::IPAddress remote_address_;
  bool is_server_{};
  std::unique_ptr<Callback> callback_;

  QuicConnectionId primary_scid_{};
  std::string alpn_wire_;

  struct OutboundStreamState {
    td::ChainBufferWriter writer_;
    td::ChainBufferReader reader_{writer_.extract_reader()};
    td::ChainBufferReader pin_{reader_.clone()};

    uint64_t acked_prefix{};

    bool fin_pending = false;
    bool fin_submitted = false;
    bool fin_acked = false;
  };

  openssl_ptr<SSL_CTX, &SSL_CTX_free> ssl_ctx_;
  openssl_ptr<SSL, &SSL_free> ssl_;
  openssl_ptr<ngtcp2_crypto_ossl_ctx, &ngtcp2_crypto_ossl_ctx_del> ossl_ctx_;
  openssl_ptr<ngtcp2_conn, &ngtcp2_conn_del> conn_;
  ngtcp2_crypto_conn_ref conn_ref_{};

  std::unordered_map<QuicStreamID, OutboundStreamState> streams_;

  // RPK (Raw Public Key) - uses Ed25519 keys for identity
  // Verification happens post-handshake via ssl_get_peer_ed25519_public_key()
  [[nodiscard]] td::Status init_tls_client_rpk(const td::Ed25519::PrivateKey& client_key, td::Slice alpn);
  [[nodiscard]] td::Status init_tls_server_rpk(const td::Ed25519::PrivateKey& server_key, td::Slice alpn);

  [[nodiscard]] td::Status init_quic_client();
  [[nodiscard]] td::Status init_quic_server(const ngtcp2_version_cid& vc);

  void setup_alpn_wire(td::Slice alpn);
  [[nodiscard]] td::Status finish_tls_setup(openssl_ptr<SSL, &SSL_free> ssl_ptr,
                                            openssl_ptr<SSL_CTX, &SSL_CTX_free> ssl_ctx_ptr, bool is_client);

  static void build_unsent_vecs(std::vector<ngtcp2_vec>& out, OutboundStreamState& st);

  [[nodiscard]] td::Status write_one_packet(UdpMessageBuffer& msg_out, QuicStreamID sid);

  ngtcp2_path make_path() const;

  static ngtcp2_tstamp now_ts();

  static ngtcp2_conn* get_pimpl_from_ref(ngtcp2_crypto_conn_ref* ref);

  static void setup_ngtcp2_callbacks(ngtcp2_callbacks& callbacks, bool is_client);

  int on_handshake_completed();
  int on_recv_stream_data(uint32_t flags, int64_t stream_id, td::Slice data);
  int on_acked_stream_data_offset(int64_t stream_id, uint64_t offset, uint64_t datalen);
  int on_stream_close(int64_t stream_id);

  static void rand_cb(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx);
  static int get_new_connection_id_cb(ngtcp2_conn* conn, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                      void* user_data);
  static int handshake_completed_cb(ngtcp2_conn* conn, void* user_data);
  static int recv_stream_data_cb(ngtcp2_conn* /*conn*/, uint32_t flags, int64_t stream_id, uint64_t offset,
                                 const uint8_t* data, size_t datalen, void* user_data, void* stream_user_data);

  static int acked_stream_data_offset_cb(ngtcp2_conn* /*conn*/, int64_t stream_id, uint64_t offset, uint64_t datalen,
                                         void* user_data, void* stream_user_data);
  static int stream_close_cb(ngtcp2_conn* /*conn*/, uint32_t flags, int64_t stream_id, uint64_t app_error_code,
                             void* user_data, void* stream_user_data);

  static int alpn_select_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                            unsigned int inlen, void* arg);
};
}  // namespace ton::quic
