#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <openssl/ssl.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "crypto/Ed25519.h"
#include "crypto/common/refcnt.hpp"
#include "ngtcp2/ngtcp2.h"
#include "ngtcp2/ngtcp2_crypto.h"
#include "ngtcp2/ngtcp2_crypto_ossl.h"
#include "td/utils/Badge.h"
#include "td/utils/Time.h"
#include "td/utils/port/UdpSocketFd.h"

#include "metrics.h"
#include "openssl-utils.h"
#include "quic-common.h"

namespace ton::quic {

inline std::chrono::nanoseconds to_chrono(ngtcp2_duration d) {
  return std::chrono::nanoseconds(d);
}

// "-206 (ERR_STREAM_ID_BLOCKED)" — the numeric code alone is opaque in logs.
inline std::string ngtcp2_err_str(ngtcp2_ssize rv) {
  return std::to_string(rv) + " (" + ngtcp2_strerror(static_cast<int>(rv)) + ")";
}

struct ServerIdentities : td::CntObject {
  std::map<std::string, ServerIdentity> by_sni;
  std::string default_sni;  // empty until the first identity is added; never empty afterwards

  bool add_identity(ServerIdentity identity);
  ServerIdentities* make_copy() const override;

  // Holds once at least one identity has been added: the default points at a real entry.
  bool has_default() const {
    return !default_sni.empty() && by_sni.contains(default_sni);
  }
};

struct QuicConnectionOptions {
  static constexpr size_t DEFAULT_INITIAL_MAX_DATA = 4 << 20;
  static constexpr size_t DEFAULT_MAX_WINDOW = 24 << 20;
  static constexpr size_t DEFAULT_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL = 4 << 20;
  static constexpr size_t DEFAULT_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE = 256 << 10;
  static constexpr size_t DEFAULT_MAX_STREAM_WINDOW = 6 << 20;
  static constexpr size_t DEFAULT_MAX_STREAMS_BIDI = 4096;
  static constexpr size_t DEFAULT_MAX_STREAMS_UNI = 4096;
  static constexpr ngtcp2_duration DEFAULT_IDLE_TIMEOUT = 15 * NGTCP2_SECONDS;
  static constexpr ngtcp2_duration DEFAULT_KEEP_ALIVE_TIMEOUT = 5 * NGTCP2_SECONDS;
  // Two round trips of handshake (stateless retry adds one) plus room for a lost packet at ngtcp2's
  // 333ms assumed initial RTT. Long enough not to abandon a slow peer, short enough that queued
  // application messages fail while they still matter.
  static constexpr ngtcp2_duration DEFAULT_HANDSHAKE_TIMEOUT = 5 * NGTCP2_SECONDS;
  // 25ms is RFC 9000's default, but mainnet validators see a ~36ms mean RTT, so advertising 25ms
  // inflates the peer's PTO (srtt + 4*rttvar + max_ack_delay) by roughly 70% and slows real loss
  // recovery. The ack threshold stays at ngtcp2's spec-conformant 2: raising it measured no benefit,
  // and deviating costs loss-detection latency on lossy paths.
  static constexpr ngtcp2_duration DEFAULT_MAX_ACK_DELAY = 10 * NGTCP2_MILLISECONDS;
  static constexpr size_t DEFAULT_ACK_THRESH = 2;

  size_t initial_max_data = DEFAULT_INITIAL_MAX_DATA;
  size_t max_window = DEFAULT_MAX_WINDOW;
  size_t initial_max_stream_data_bidi_local = DEFAULT_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL;
  size_t initial_max_stream_data_bidi_remote = DEFAULT_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE;
  size_t max_stream_window = DEFAULT_MAX_STREAM_WINDOW;
  size_t max_streams_bidi = DEFAULT_MAX_STREAMS_BIDI;
  // Messages travel on unidirectional streams, avoiding an application-level receipt. Transport
  // stream credit can be returned together with ACKs.
  size_t max_streams_uni = DEFAULT_MAX_STREAMS_UNI;
  size_t initial_max_stream_data_uni = DEFAULT_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE;
  ngtcp2_duration idle_timeout = DEFAULT_IDLE_TIMEOUT;
  ngtcp2_duration keep_alive_timeout = DEFAULT_KEEP_ALIVE_TIMEOUT;
  ngtcp2_duration handshake_timeout = DEFAULT_HANDSHAKE_TIMEOUT;
  ngtcp2_duration max_ack_delay = DEFAULT_MAX_ACK_DELAY;
  size_t ack_thresh = DEFAULT_ACK_THRESH;
  CongestionControlAlgo cc_algo = CongestionControlAlgo::Bbr;
  // RFC 9221 DATAGRAM frame size advertised to the peer; 0 disables DATAGRAM messages.
  td::uint32 max_datagram_frame_size = 0;
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
  std::string token{};

  static td::Result<VersionCid> from_datagram(td::Slice datagram) {
    if (datagram.size() == 0) {
      return td::Status::Error("empty datagram");
    }

    ngtcp2_version_cid vc;
    int rv = ngtcp2_pkt_decode_version_cid(&vc, reinterpret_cast<const uint8_t*>(datagram.data()), datagram.size(),
                                           QuicConnectionId::MAX_SIZE);
    if (rv != 0) {
      return td::Status::Error("failed to decode version_cid");
    }
    return from_parts(vc.version, vc.dcid, vc.dcidlen, vc.scid, vc.scidlen);
  }

  static td::Result<VersionCid> from_initial_datagram(td::Slice datagram) {
    if (datagram.empty()) {
      return td::Status::Error("empty datagram");
    }

    ngtcp2_pkt_hd hd;
    int rv = ngtcp2_accept(&hd, reinterpret_cast<const uint8_t*>(datagram.data()), datagram.size());
    if (rv != 0) {
      return td::Status::Error("packet is not acceptable as an initial packet");
    }
    if (hd.type != NGTCP2_PKT_INITIAL) {
      return td::Status::Error("first packet is not an initial packet");
    }

    td::Slice token;
    if (hd.token != nullptr && hd.tokenlen > 0) {
      token = td::Slice(reinterpret_cast<const char*>(hd.token), hd.tokenlen);
    }
    return from_parts(hd.version, hd.dcid.data, hd.dcid.datalen, hd.scid.data, hd.scid.datalen, token);
  }

 private:
  static td::Result<VersionCid> from_parts(td::uint32 version, const uint8_t* dcid_data, size_t dcid_size,
                                           const uint8_t* scid_data, size_t scid_size, td::Slice token = {}) {
    TRY_RESULT(scid, QuicConnectionId::from_raw(scid_data, scid_size));
    TRY_RESULT(dcid, QuicConnectionId::from_raw(dcid_data, dcid_size));
    return VersionCid{.version = version, .dcid = dcid, .scid = scid, .token = token.str()};
  }
};

struct ServerInitialInfo {
  VersionCid packet;
  QuicConnectionId original_dcid{};
  std::optional<QuicConnectionId> retry_scid;
};

struct QuicConnectionPImpl {
  enum class ExpiryAction {
    None,
    ScheduleWrite,
    IdleClose,
    HandshakeTimeout,
    Close,
  };
  struct InitialCidState {
    QuicConnectionId primary_scid;
    td::vector<QuicConnectionId> scids;
  };
  class Callback {
   public:
    struct HandshakeCompletedEvent {
      td::SecureString peer_public_key;   // Ed25519 public key (32 bytes), empty if not available
      td::SecureString local_public_key;  // Ed25519 public key (32 bytes) of the local identity the
                                          // handshake actually used. For outbound connections it's
                                          // the client key passed to create_client. For inbound it's
                                          // the identity selected by SNI (or the default).
    };
    struct StreamDataEvent {
      QuicStreamID sid = 0;
      td::BufferSlice data;
      bool fin = false;
    };

    virtual void set_connection_id(QuicConnectionId cid) = 0;
    virtual void on_local_cid_issued(QuicConnectionId cid) = 0;
    virtual void on_local_cid_retired(QuicConnectionId cid) = 0;
    virtual void on_handshake_completed(HandshakeCompletedEvent event) = 0;
    virtual td::Status on_stream_data(StreamDataEvent event) = 0;
    virtual void on_stream_closed(StreamCloseEvent event) = 0;

    virtual ~Callback() = default;
  };

  QuicConnectionPImpl(td::Badge<QuicConnectionPImpl>, const td::IPAddress& local_address,
                      const td::IPAddress& remote_address, std::unique_ptr<Callback> callback,
                      QuicConnectionOptions options)
      : local_address_(local_address)
      , remote_address_(remote_address)
      , callback_(std::move(callback))
      , options_(options) {
  }

  [[nodiscard]] static td::Result<std::unique_ptr<QuicConnectionPImpl>> create_client(
      const td::IPAddress& local_address, const td::IPAddress& remote_address,
      const td::Ed25519::PrivateKey& client_key, td::Slice alpn, td::Slice sni, std::unique_ptr<Callback> callback,
      QuicConnectionOptions options = {});

  [[nodiscard]] static td::Result<std::unique_ptr<QuicConnectionPImpl>> create_server(
      const td::IPAddress& local_address, const td::IPAddress& remote_address, td::Ref<ServerIdentities> identities,
      td::Slice alpn, const ServerInitialInfo& initial, std::unique_ptr<Callback> callback,
      QuicConnectionOptions options = {});

  [[nodiscard]] td::Status produce_egress(UdpMessageBuffer& msg_out, size_t max_packets);
  [[nodiscard]] td::Status handle_ingress(const UdpMessageBuffer& msg_in, UdpMessageBuffer& close_msg_out);

  [[nodiscard]] td::Timestamp get_expiry_timestamp() const;
  [[nodiscard]] bool is_expired() const;
  [[nodiscard]] td::Result<ExpiryAction> handle_expiry();

  [[nodiscard]] td::Result<InitialCidState> take_initial_cid_state();

  void shutdown_stream(QuicStreamID sid);
  // Releases one peer-opened unidirectional stream after the upper layer has processed its payload.
  // Calls are exactly once and correspond one-for-one with peer uni close events. Returns whether
  // the release put a MAX_STREAMS on the wire, so the caller can skip the ones that did not.
  [[nodiscard]] bool release_peer_uni_stream_credit();
  void set_stream_receive_credit_from_max_size(QuicStreamID sid, td::uint64 max_size);

  [[nodiscard]] td::Result<QuicStreamID> open_stream(StreamDirection direction, td::BufferSlice data, bool fin);
  // Whether the peer's initial transport parameters allowed unidirectional streams at all. A peer
  // from before messages moved onto them advertises none and, receiving none, never sends a
  // MAX_STREAMS_UNI either -- so opening one would fail forever rather than eventually, and its
  // messages have to ride bidirectional streams instead.
  [[nodiscard]] bool peer_advertised_initial_uni_credit() const;
  [[nodiscard]] td::Status buffer_stream(QuicStreamID sid, td::BufferSlice data, bool fin);

  [[nodiscard]] bool can_send_datagram(size_t size) const;
  void send_datagram(td::BufferSlice data);
  [[nodiscard]] ngtcp2_conn_info get_conn_info() const;
  [[nodiscard]] size_t get_last_packet_streams() const {
    return last_packet_streams_;
  }

  ~QuicConnectionPImpl() {
    if (ssl_) {
      SSL_set_app_data(ssl_.get(), nullptr);
    }
  }

  QuicConnectionMetrics get_stats(TransportStats& transport_stats);
  // Records the reject from the last handle_ingress() call unless ngtcp2 reported a discard during
  // that call. ngtcp2 exposes only aggregate per-call counters, so a rare buffered-packet
  // interleaving can merge the reject with an unrelated discard; see metrics/METRICS.md.
  void account_ingress_reject(TransportStats& transport_stats);

 private:
  td::IPAddress local_address_;
  td::IPAddress remote_address_;
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

  [[nodiscard]] td::Status buffer_stream(QuicStreamID sid, OutboundStreamState& st, td::BufferSlice data, bool fin);

  // Announces the credit of one closed peer stream, batching announcements. Returns whether this
  // one went on the wire.
  bool extend_peer_stream_credit(StreamDirection direction);

  openssl_ptr<SSL_CTX, &SSL_CTX_free> ssl_ctx_;
  openssl_ptr<SSL, &SSL_free> ssl_;
  openssl_ptr<ngtcp2_crypto_ossl_ctx, &ngtcp2_crypto_ossl_ctx_del> ossl_ctx_;
  openssl_ptr<ngtcp2_conn, &ngtcp2_conn_del> conn_;
  ngtcp2_crypto_conn_ref conn_ref_{};
  bool local_cid_callbacks_enabled_{false};

  size_t sids_encountered = 0;
  // Peer-opened unidirectional streams the transport has closed but the upper layer has not yet
  // processed. The close event and release call are an exactly-once counting contract.
  size_t held_peer_uni_streams_ = 0;
  // Credit of closed peer streams not announced yet, per direction. See extend_peer_stream_credit().
  size_t withheld_bidi_credit_ = 0;
  size_t withheld_uni_credit_ = 0;
  td::uint64 last_pkt_discarded_ = 0;
  bool last_ingress_discarded_ = false;
  metrics::Labeled<metrics::Counter, metrics::Direction> stream_bytes_;
  metrics::Labeled<metrics::Counter, metrics::Direction> datagrams_;
  std::unordered_map<QuicStreamID, OutboundStreamState> streams_;
  std::deque<QuicStreamID> ready_streams_;
  bool stop_packet_aggregation_ = false;
  static constexpr size_t MAX_PENDING_DATAGRAMS = 4096;
  std::deque<td::BufferSlice> pending_datagrams_;
  QuicStreamID write_sid_ = -1;
  std::vector<ngtcp2_vec> write_datav_;
  size_t last_packet_streams_ = 0;

  ngtcp2_conn* conn() const {
    CHECK(conn_);
    return conn_.get();
  }

  [[nodiscard]] td::Status init_tls_client_rpk(const td::Ed25519::PrivateKey& client_key, td::Slice alpn,
                                               td::Slice sni);
  [[nodiscard]] td::Status init_tls_server_rpk(td::Ref<ServerIdentities> identities, td::Slice alpn);

  [[nodiscard]] td::Status init_quic_client();
  [[nodiscard]] td::Status init_quic_server(const ServerInitialInfo& initial);
  void finish_quic_init(const QuicConnectionId& scid);

  [[nodiscard]] td::SecureString extract_peer_ed25519_key() const;
  [[nodiscard]] td::SecureString extract_local_ed25519_key() const;

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

  void commit_write(UdpMessageBuffer& msg_out, size_t n_write, size_t gso_size, const ngtcp2_path& path);
  void write_connection_close(UdpMessageBuffer& close_out, int liberr);
  void prepare_stream_write(QuicStreamID sid, bool may_pad, StreamWriteContext& ctx, std::vector<ngtcp2_vec>& datav);
  void finish_stream_write(QuicStreamID sid, const StreamWriteContext& ctx, ngtcp2_ssize pdatalen);
  void start_batch();
  QuicStreamID next_ready_stream_id();

  void finish_batch();
  ngtcp2_ssize write_frames_to_packet(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest, size_t destlen,
                                      bool may_pad, ngtcp2_tstamp ts);
  ngtcp2_ssize write_datagrams_to_packet(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest, size_t destlen,
                                         bool may_pad, ngtcp2_tstamp ts);
  ngtcp2_ssize write_streams_to_packet(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest, size_t destlen,
                                       bool may_pad, ngtcp2_tstamp ts);
  static ngtcp2_ssize write_pkt_cb(ngtcp2_conn* conn, ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest,
                                   size_t destlen, ngtcp2_tstamp ts, void* user_data);
  static int extend_max_stream_data_cb(ngtcp2_conn* conn, int64_t stream_id, uint64_t max_data, void* user_data,
                                       void* stream_user_data);

  ngtcp2_path make_path() const;
  ngtcp2_path make_path(const td::IPAddress& remote) const;

  static ngtcp2_tstamp now_ts();

  static ngtcp2_conn* get_pimpl_from_ref(ngtcp2_crypto_conn_ref* ref);

  static void setup_ngtcp2_callbacks(ngtcp2_callbacks& callbacks, bool is_client);

  int on_handshake_completed();
  int on_recv_stream_data(uint32_t flags, int64_t stream_id, td::Slice data);
  int on_acked_stream_data_offset(int64_t stream_id, uint64_t offset, uint64_t datalen);
  int on_stream_close(int64_t stream_id, bool clean);
  int on_extend_max_stream_data(QuicStreamID sid);
  int on_get_new_connection_id(ngtcp2_cid* cid, uint8_t* token, size_t cidlen);
  int on_remove_connection_id(const ngtcp2_cid* cid);

  static void rand_cb(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx);
  static int get_new_connection_id_cb(ngtcp2_conn* conn, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                      void* user_data);
  static int remove_connection_id_cb(ngtcp2_conn* conn, const ngtcp2_cid* cid, void* user_data);
  static int handshake_completed_cb(ngtcp2_conn* conn, void* user_data);
  static int recv_stream_data_cb(ngtcp2_conn* /*conn*/, uint32_t flags, int64_t stream_id, uint64_t offset,
                                 const uint8_t* data, size_t datalen, void* user_data, void* stream_user_data);

  static int acked_stream_data_offset_cb(ngtcp2_conn* /*conn*/, int64_t stream_id, uint64_t offset, uint64_t datalen,
                                         void* user_data, void* stream_user_data);
  static int stream_close_cb(ngtcp2_conn* /*conn*/, uint32_t flags, int64_t stream_id, uint64_t app_error_code,
                             void* user_data, void* stream_user_data);
  static int recv_datagram_cb(ngtcp2_conn* /*conn*/, uint32_t flags, const uint8_t* data, size_t datalen,
                              void* user_data);

  static int alpn_select_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                            unsigned int inlen, void* arg);
  static int sni_select_cb(SSL* ssl, int* ad, void* arg);

  td::Ref<ServerIdentities> server_identities_;
};
}  // namespace ton::quic
