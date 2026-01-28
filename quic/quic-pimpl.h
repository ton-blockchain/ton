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
#include "td/utils/Random.h"
#include "td/utils/Time.h"
#include "td/utils/crypto.h"
#include "td/utils/port/UdpSocketFd.h"

#include "openssl-utils.h"
#include "quic-common.h"

namespace ton::quic {

struct QuicConnectionOptions {
  static constexpr size_t DEFAULT_MAX_WINDOW = 24 << 20;
  static constexpr size_t DEFAULT_MAX_STREAM_WINDOW = 6 << 20;
  static constexpr size_t DEFAULT_MAX_STREAMS_BIDI = 1024;
  static constexpr ngtcp2_duration DEFAULT_IDLE_TIMEOUT = 15 * NGTCP2_SECONDS;
  static constexpr ngtcp2_duration DEFAULT_KEEP_ALIVE_TIMEOUT = 5 * NGTCP2_SECONDS;

  size_t max_window = DEFAULT_MAX_WINDOW;
  size_t max_stream_window = DEFAULT_MAX_STREAM_WINDOW;
  size_t max_streams_bidi = DEFAULT_MAX_STREAMS_BIDI;
  ngtcp2_duration idle_timeout = DEFAULT_IDLE_TIMEOUT;
  ngtcp2_duration keep_alive_timeout = DEFAULT_KEEP_ALIVE_TIMEOUT;
  CongestionControlAlgo cc_algo = CongestionControlAlgo::Bbr;
};

struct QuicConnectionIdAccess {
  static ngtcp2_cid to_ngtcp2(const QuicConnectionId& cid) {
    ngtcp2_cid result{};
    auto slice = cid.as_slice();
    result.datalen = slice.size();
    std::memcpy(result.data, slice.data(), slice.size());
    return result;
  }
  static QuicConnectionId from_ngtcp2(const ngtcp2_cid& cid) {
    return QuicConnectionId::from_raw(cid.data, cid.datalen).move_as_ok();
  }
};

struct VersionCid {
  td::uint32 version{};
  QuicConnectionId dcid{};
  QuicConnectionId scid{};

  static td::Result<VersionCid> from_datagram(td::Slice datagram) {
    ngtcp2_version_cid vc;
    int rv = ngtcp2_pkt_decode_version_cid(&vc, reinterpret_cast<const uint8_t*>(datagram.data()), datagram.size(),
                                           QuicConnectionId::MAX_SIZE);
    if (rv != 0) {
      return td::Status::Error("failed to decode version_cid");
    }
    TRY_RESULT(scid, QuicConnectionId::from_raw(vc.scid, vc.scidlen));
    TRY_RESULT(dcid, QuicConnectionId::from_raw(vc.dcid, vc.dcidlen));
    return VersionCid{.version = vc.version, .dcid = dcid, .scid = scid};
  }
};

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
    virtual void on_handshake_completed(HandshakeCompletedEvent event) = 0;
    virtual td::Status on_stream_data(StreamDataEvent event) = 0;
    virtual void on_stream_closed(QuicStreamID sid) = 0;

    virtual ~Callback() = default;
  };

  constexpr static size_t DEFAULT_WINDOW = 1 << 20;
  constexpr static size_t CID_LENGTH = 16;

  struct PrivateTag {};

  QuicConnectionPImpl(PrivateTag, const td::IPAddress& local_address, const td::IPAddress& remote_address,
                      bool is_server, std::unique_ptr<Callback> callback, QuicConnectionOptions options)
      : local_address_(local_address)
      , remote_address_(remote_address)
      , is_server_(is_server)
      , callback_(std::move(callback))
      , options_(options) {
  }

  [[nodiscard]] static td::Result<std::unique_ptr<QuicConnectionPImpl>> create_client(
      const td::IPAddress& local_address, const td::IPAddress& remote_address,
      const td::Ed25519::PrivateKey& client_key, td::Slice alpn, std::unique_ptr<Callback> callback,
      QuicConnectionOptions options = {});

  [[nodiscard]] static td::Result<std::unique_ptr<QuicConnectionPImpl>> create_server(
      const td::IPAddress& local_address, const td::IPAddress& remote_address,
      const td::Ed25519::PrivateKey& server_key, td::Slice alpn, const VersionCid& vc,
      std::unique_ptr<Callback> callback, QuicConnectionOptions options = {});

  [[nodiscard]] td::Status produce_egress(UdpMessageBuffer& msg_out, bool use_gso, size_t max_packets);
  [[nodiscard]] td::Status handle_ingress(const UdpMessageBuffer& msg_in);

  [[nodiscard]] td::Timestamp get_expiry_timestamp() const;
  [[nodiscard]] bool is_expired() const;
  [[nodiscard]] td::Result<ExpiryAction> handle_expiry();

  [[nodiscard]] QuicConnectionId get_primary_scid() const;

  void shutdown_stream(QuicStreamID sid);

  [[nodiscard]] td::Result<QuicStreamID> open_stream();
  [[nodiscard]] td::Status buffer_stream(QuicStreamID sid, td::BufferSlice data, bool fin);
  [[nodiscard]] ngtcp2_conn_info get_conn_info() const;
  [[nodiscard]] size_t get_last_packet_streams() const {
    return last_packet_streams_;
  }

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
  QuicConnectionOptions options_;

  QuicConnectionId primary_scid_{};
  std::string alpn_wire_;

  struct OutboundStreamState {
    td::ChainBufferWriter writer_;
    td::ChainBufferReader reader_{writer_.extract_reader()};
    td::ChainBufferReader pin_{reader_.clone()};

    uint64_t acked_prefix{};

    bool is_blocked = false;
    bool is_write_closed = false;
    bool fin_pending = false;
    bool fin_submitted = false;
    bool fin_acked = false;
    bool in_ready_queue = false;
  };

  openssl_ptr<SSL_CTX, &SSL_CTX_free> ssl_ctx_;
  openssl_ptr<SSL, &SSL_free> ssl_;
  openssl_ptr<ngtcp2_crypto_ossl_ctx, &ngtcp2_crypto_ossl_ctx_del> ossl_ctx_;
  openssl_ptr<ngtcp2_conn, &ngtcp2_conn_del> conn_;
  ngtcp2_crypto_conn_ref conn_ref_{};

  std::unordered_map<QuicStreamID, OutboundStreamState> streams_;
  std::deque<QuicStreamID> ready_streams_;
  QuicStreamID write_sid_ = -1;
  bool write_padding_ = false;
  std::vector<ngtcp2_vec> write_datav_;
  size_t last_packet_streams_ = 0;

  ngtcp2_conn* conn() const {
    CHECK(conn_);
    return conn_.get();
  }

  // RPK (Raw Public Key) - uses Ed25519 keys for identity
  // Verification happens post-handshake via ssl_get_peer_ed25519_public_key()
  [[nodiscard]] td::Status init_tls_client_rpk(const td::Ed25519::PrivateKey& client_key, td::Slice alpn);
  [[nodiscard]] td::Status init_tls_server_rpk(const td::Ed25519::PrivateKey& server_key, td::Slice alpn);

  [[nodiscard]] td::Status init_quic_client();
  [[nodiscard]] td::Status init_quic_server(const VersionCid& vc);

  static void setup_settings_and_params(ngtcp2_settings& settings, ngtcp2_transport_params& params,
                                        const QuicConnectionOptions& options);

  void setup_alpn_wire(td::Slice alpn);
  [[nodiscard]] td::Status finish_tls_setup(openssl_ptr<SSL, &SSL_free> ssl_ptr,
                                            openssl_ptr<SSL_CTX, &SSL_CTX_free> ssl_ctx_ptr, bool is_client);

  static void build_unsent_vecs(std::vector<ngtcp2_vec>& out, OutboundStreamState& st);
  static bool is_stream_ready(const OutboundStreamState& st);
  void mark_stream_ready(QuicStreamID sid, OutboundStreamState& st);
  QuicStreamID pop_ready_stream();
  void try_enqueue_stream(QuicStreamID sid);

  struct StreamWriteContext {
    uint32_t flags = 0;
    uint64_t unsent_before = 0;
  };

  static td::Status clear_out(UdpMessageBuffer& msg_out);
  void commit_write(UdpMessageBuffer& msg_out, size_t n_write, size_t gso_size);
  void prepare_stream_write(QuicStreamID sid, bool padding, StreamWriteContext& ctx, std::vector<ngtcp2_vec>& datav);
  void finish_stream_write(QuicStreamID sid, const StreamWriteContext& ctx, ngtcp2_ssize pdatalen);
  void start_batch();
  QuicStreamID next_ready_stream_id();

  void finish_batch();
  ngtcp2_ssize write_streams_to_packet(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest, size_t destlen,
                                       bool padding, ngtcp2_tstamp ts);
  ngtcp2_ssize write_pkt_aggregate(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest, size_t destlen,
                                   ngtcp2_tstamp ts);
  static ngtcp2_ssize write_pkt_cb(ngtcp2_conn* conn, ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest,
                                   size_t destlen, ngtcp2_tstamp ts, void* user_data);
  static int extend_max_stream_data_cb(ngtcp2_conn* conn, int64_t stream_id, uint64_t max_data, void* user_data,
                                       void* stream_user_data);

  ngtcp2_path make_path() const;

  static ngtcp2_tstamp now_ts();

  static ngtcp2_conn* get_pimpl_from_ref(ngtcp2_crypto_conn_ref* ref);

  static void setup_ngtcp2_callbacks(ngtcp2_callbacks& callbacks, bool is_client);

  int on_handshake_completed();
  int on_recv_stream_data(uint32_t flags, int64_t stream_id, td::Slice data);
  int on_acked_stream_data_offset(int64_t stream_id, uint64_t offset, uint64_t datalen);
  int on_stream_close(int64_t stream_id);
  int on_extend_max_stream_data(QuicStreamID sid, uint64_t max_data);

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
