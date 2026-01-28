#include <algorithm>
#include <cstring>
#include <limits>
#include <openssl/ssl.h>
#include <vector>

#include "td/utils/Random.h"
#include "td/utils/logging.h"
#include "td/utils/port/UdpSocketFd.h"

#include "openssl-utils.h"
#include "quic-common.h"
#include "quic-pimpl.h"

namespace ton::quic {

static constexpr ngtcp2_tstamp NGTCP2_TSTAMP_INF = std::numeric_limits<ngtcp2_tstamp>::max();

static ngtcp2_tstamp to_ngtcp2_tstamp(td::Timestamp ts) {
  if (ts.at() < 0) {
    return 0;
  }
  constexpr double MAX_SEC = static_cast<double>(NGTCP2_TSTAMP_INF - 1) / 1e9;
  if (!ts || ts.at() >= MAX_SEC) {
    return NGTCP2_TSTAMP_INF;
  }
  return static_cast<ngtcp2_tstamp>(ts.at() * 1e9);
}

static td::Timestamp from_ngtcp2_tstamp(ngtcp2_tstamp ns) {
  if (ns == NGTCP2_TSTAMP_INF) {
    return td::Timestamp::never();
  }
  return td::Timestamp::at(static_cast<double>(ns) * 1e-9);
}

td::Result<std::unique_ptr<QuicConnectionPImpl>> QuicConnectionPImpl::create_client(
    const td::IPAddress& local_address, const td::IPAddress& remote_address, const td::Ed25519::PrivateKey& client_key,
    td::Slice alpn, std::unique_ptr<Callback> callback, QuicConnectionOptions options) {
  auto p_impl = std::make_unique<QuicConnectionPImpl>(PrivateTag{}, local_address, remote_address, false,
                                                      std::move(callback), options);

  TRY_STATUS(p_impl->init_tls_client_rpk(client_key, alpn));
  TRY_STATUS(p_impl->init_quic_client());

  p_impl->callback_->set_connection_id(p_impl->get_primary_scid());

  return std::move(p_impl);
}

td::Result<std::unique_ptr<QuicConnectionPImpl>> QuicConnectionPImpl::create_server(
    const td::IPAddress& local_address, const td::IPAddress& remote_address, const td::Ed25519::PrivateKey& server_key,
    td::Slice alpn, const VersionCid& vc, std::unique_ptr<Callback> callback, QuicConnectionOptions options) {
  auto p_impl = std::make_unique<QuicConnectionPImpl>(PrivateTag{}, local_address, remote_address, true,
                                                      std::move(callback), options);

  TRY_STATUS(p_impl->init_tls_server_rpk(server_key, alpn));
  TRY_STATUS(p_impl->init_quic_server(vc));

  p_impl->callback_->set_connection_id(p_impl->get_primary_scid());

  return std::move(p_impl);
}

void QuicConnectionPImpl::setup_alpn_wire(td::Slice alpn) {
  alpn_wire_ = std::string(1, td::narrow_cast<char>(alpn.size())) + alpn.str();
}

td::Status QuicConnectionPImpl::finish_tls_setup(openssl_ptr<SSL, &SSL_free> ssl_ptr,
                                                 openssl_ptr<SSL_CTX, &SSL_CTX_free> ssl_ctx_ptr, bool is_client) {
  conn_ref_.get_conn = get_pimpl_from_ref;
  conn_ref_.user_data = this;
  SSL_set_app_data(ssl_ptr.get(), &conn_ref_);

  if (is_client) {
    if (ngtcp2_crypto_ossl_configure_client_session(ssl_ptr.get()) != 0) {
      return td::Status::Error("ngtcp2_crypto_ossl_configure_client_session failed");
    }
  } else {
    if (ngtcp2_crypto_ossl_configure_server_session(ssl_ptr.get()) != 0) {
      return td::Status::Error("ngtcp2_crypto_ossl_configure_server_session failed");
    }
  }

  ngtcp2_crypto_ossl_ctx* ossl_ctx = nullptr;
  if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx, ssl_ptr.get()) != 0) {
    return td::Status::Error("ngtcp2_crypto_ossl_ctx_new failed");
  }
  ossl_ctx_.reset(ossl_ctx);

  ssl_ctx_ = std::move(ssl_ctx_ptr);
  ssl_ = std::move(ssl_ptr);
  return td::Status::OK();
}

static td::Status setup_rpk_context(SSL_CTX* ssl_ctx, const td::Ed25519::PrivateKey& key) {
  SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);
  SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, [](int, X509_STORE_CTX*) { return 1; });

  static const unsigned char cert_types[] = {TLSEXT_cert_type_rpk};
  OPENSSL_CHECK_OK(SSL_CTX_set1_server_cert_type(ssl_ctx, cert_types, sizeof(cert_types)),
                   "Failed to enable server RPK");
  OPENSSL_CHECK_OK(SSL_CTX_set1_client_cert_type(ssl_ctx, cert_types, sizeof(cert_types)),
                   "Failed to enable client RPK");

  auto key_bytes = key.as_octet_string();
  OPENSSL_MAKE_PTR(evp_key, EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, key_bytes.as_slice().ubegin(), 32),
                   EVP_PKEY_free, "Failed to create Ed25519 key from raw bytes");
  OPENSSL_CHECK_OK(SSL_CTX_use_PrivateKey(ssl_ctx, evp_key.get()), "Failed to set private key");
  return td::Status::OK();
}

int QuicConnectionPImpl::alpn_select_cb(SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                                        unsigned int inlen, void* arg) {
  auto* wire = static_cast<std::string*>(arg);
  unsigned char* sel = nullptr;
  if (SSL_select_next_proto(&sel, outlen, reinterpret_cast<const unsigned char*>(wire->data()),
                            static_cast<unsigned int>(wire->size()), in, inlen) == OPENSSL_NPN_NEGOTIATED) {
    *out = sel;
    return SSL_TLSEXT_ERR_OK;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

td::Status QuicConnectionPImpl::init_tls_client_rpk(const td::Ed25519::PrivateKey& client_key, td::Slice alpn) {
  OPENSSL_MAKE_PTR(ssl_ctx_ptr, SSL_CTX_new(TLS_client_method()), SSL_CTX_free, "Failed to create TLS client context");
  TRY_STATUS(setup_rpk_context(ssl_ctx_ptr.get(), client_key));
  setup_alpn_wire(alpn);

  OPENSSL_MAKE_PTR(ssl_ptr, SSL_new(ssl_ctx_ptr.get()), SSL_free, "Failed to create SSL session");
  SSL_set_connect_state(ssl_ptr.get());

  SSL_set_alpn_protos(ssl_ptr.get(), reinterpret_cast<const unsigned char*>(alpn_wire_.c_str()),
                      static_cast<unsigned int>(alpn_wire_.size()));

  return finish_tls_setup(std::move(ssl_ptr), std::move(ssl_ctx_ptr), true);
}

td::Status QuicConnectionPImpl::init_tls_server_rpk(const td::Ed25519::PrivateKey& server_key, td::Slice alpn) {
  OPENSSL_MAKE_PTR(ssl_ctx_ptr, SSL_CTX_new(TLS_server_method()), SSL_CTX_free, "Failed to create TLS server context");
  TRY_STATUS(setup_rpk_context(ssl_ctx_ptr.get(), server_key));
  setup_alpn_wire(alpn);

  SSL_CTX_set_alpn_select_cb(ssl_ctx_ptr.get(), alpn_select_cb, &alpn_wire_);

  OPENSSL_MAKE_PTR(ssl_ptr, SSL_new(ssl_ctx_ptr.get()), SSL_free, "Failed to create SSL session");
  SSL_set_accept_state(ssl_ptr.get());

  return finish_tls_setup(std::move(ssl_ptr), std::move(ssl_ctx_ptr), false);
}

void QuicConnectionPImpl::setup_settings_and_params(ngtcp2_settings& settings, ngtcp2_transport_params& params,
                                                    const QuicConnectionOptions& options) {
  ngtcp2_settings_default(&settings);
  settings.initial_ts = now_ts();
  settings.max_window = options.max_window;
  settings.max_stream_window = options.max_stream_window;

  switch (options.cc_algo) {
    case CongestionControlAlgo::Reno:
      settings.cc_algo = NGTCP2_CC_ALGO_RENO;
      break;
    case CongestionControlAlgo::Bbr:
      settings.cc_algo = NGTCP2_CC_ALGO_BBR;
      break;
    case CongestionControlAlgo::Cubic:
    default:
      settings.cc_algo = NGTCP2_CC_ALGO_CUBIC;
      break;
  }

  ngtcp2_transport_params_default(&params);
  params.max_idle_timeout = options.idle_timeout;
  params.initial_max_streams_bidi = options.max_streams_bidi;
  params.initial_max_stream_data_bidi_remote = options.max_stream_window;
  params.initial_max_stream_data_bidi_local = options.max_stream_window;
  params.initial_max_data = options.max_window;
}

void QuicConnectionPImpl::setup_ngtcp2_callbacks(ngtcp2_callbacks& callbacks, bool is_client) {
  if (is_client) {
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
  } else {
    callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
  }
  callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
  callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
  callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
  callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
  callbacks.update_key = ngtcp2_crypto_update_key_cb;
  callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
  callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
  callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
  callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;

  callbacks.rand = rand_cb;
  callbacks.get_new_connection_id = get_new_connection_id_cb;
  callbacks.handshake_completed = handshake_completed_cb;
  callbacks.recv_stream_data = recv_stream_data_cb;
  callbacks.acked_stream_data_offset = acked_stream_data_offset_cb;
  callbacks.stream_close = stream_close_cb;
  callbacks.extend_max_stream_data = extend_max_stream_data_cb;
}

td::Status QuicConnectionPImpl::init_quic_client() {
  ngtcp2_callbacks callbacks{};
  setup_ngtcp2_callbacks(callbacks, true);

  ngtcp2_settings settings;
  ngtcp2_transport_params params;
  setup_settings_and_params(settings, params, options_);

  auto dcid = QuicConnectionId::random();
  auto scid = QuicConnectionId::random();
  auto dcid_raw = QuicConnectionIdAccess::to_ngtcp2(dcid);
  auto scid_raw = QuicConnectionIdAccess::to_ngtcp2(scid);

  ngtcp2_path path = make_path();

  ngtcp2_conn* new_conn = nullptr;
  int rv = ngtcp2_conn_client_new(&new_conn, &dcid_raw, &scid_raw, &path, NGTCP2_PROTO_VER_V1, &callbacks, &settings,
                                  &params, nullptr, this);

  if (rv != 0) {
    return td::Status::Error("ngtcp2_conn_client_new failed");
  }
  conn_.reset(new_conn);

  primary_scid_ = scid;

  ngtcp2_conn_set_tls_native_handle(conn(), ossl_ctx_.get());
  ngtcp2_conn_set_keep_alive_timeout(conn(), options_.keep_alive_timeout);

  return td::Status::OK();
}

td::Status QuicConnectionPImpl::init_quic_server(const VersionCid& vc) {
  ngtcp2_callbacks callbacks{};
  setup_ngtcp2_callbacks(callbacks, false);

  ngtcp2_settings settings;
  ngtcp2_transport_params params;
  setup_settings_and_params(settings, params, options_);

  params.original_dcid_present = 1;
  params.original_dcid = QuicConnectionIdAccess::to_ngtcp2(vc.dcid);

  auto client_scid = QuicConnectionIdAccess::to_ngtcp2(vc.scid);
  auto server_scid = QuicConnectionId::random();
  auto server_scid_raw = QuicConnectionIdAccess::to_ngtcp2(server_scid);

  ngtcp2_path path = make_path();

  ngtcp2_conn* new_conn = nullptr;
  int rv = ngtcp2_conn_server_new(&new_conn, &client_scid, &server_scid_raw, &path, vc.version, &callbacks, &settings,
                                  &params, nullptr, this);
  if (rv != 0) {
    return td::Status::Error(PSTRING() << "ngtcp2_conn_server_new failed: " << rv);
  }
  conn_.reset(new_conn);

  primary_scid_ = server_scid;

  ngtcp2_conn_set_tls_native_handle(conn(), ossl_ctx_.get());
  ngtcp2_conn_set_keep_alive_timeout(conn(), options_.keep_alive_timeout);
  return td::Status::OK();
}

void QuicConnectionPImpl::build_unsent_vecs(std::vector<ngtcp2_vec>& out, OutboundStreamState& st) {
  out.clear();
  auto it = st.reader_.clone();
  // currently we usually have just one chunk, so this approach is perfectly fine
  while (!it.empty()) {
    auto head = it.prepare_read();
    ngtcp2_vec v{};
    v.base = const_cast<td::uint8*>(head.ubegin());
    v.len = head.size();
    out.push_back(v);
    it.confirm_read(head.size());
  }
}

bool QuicConnectionPImpl::is_stream_ready(const OutboundStreamState& st) {
  return !st.is_blocked && !st.is_write_closed && (!st.reader_.empty() || st.fin_pending);
}

void QuicConnectionPImpl::mark_stream_ready(QuicStreamID sid, OutboundStreamState& st) {
  if (!st.in_ready_queue && is_stream_ready(st)) {
    ready_streams_.push_back(sid);
    st.in_ready_queue = true;
  }
}

QuicStreamID QuicConnectionPImpl::pop_ready_stream() {
  while (!ready_streams_.empty()) {
    auto sid = ready_streams_.front();
    ready_streams_.pop_front();
    auto it = streams_.find(sid);
    if (it == streams_.end()) {
      continue;
    }
    auto& st = it->second;
    st.in_ready_queue = false;
    if (is_stream_ready(st)) {
      return sid;
    }
  }
  return -1;
}

void QuicConnectionPImpl::try_enqueue_stream(QuicStreamID sid) {
  if (sid == -1) {
    return;
  }
  auto it = streams_.find(sid);
  if (it == streams_.end()) {
    return;
  }
  mark_stream_ready(sid, it->second);
}

ngtcp2_path QuicConnectionPImpl::make_path() const {
  ngtcp2_path path{};
  path.local.addr = const_cast<ngtcp2_sockaddr*>(local_address_.get_sockaddr());
  path.local.addrlen = static_cast<ngtcp2_socklen>(local_address_.get_sockaddr_len());
  path.remote.addr = const_cast<ngtcp2_sockaddr*>(remote_address_.get_sockaddr());
  path.remote.addrlen = static_cast<ngtcp2_socklen>(remote_address_.get_sockaddr_len());
  return path;
}

td::Status QuicConnectionPImpl::clear_out(UdpMessageBuffer& msg_out) {
  msg_out.storage.truncate(0);
  msg_out.gso_size = 0;
  return td::Status::OK();
}

void QuicConnectionPImpl::commit_write(UdpMessageBuffer& msg_out, size_t n_write, size_t gso_size) {
  msg_out.storage.truncate(n_write);
  msg_out.address = remote_address_;
  msg_out.gso_size = gso_size;
}

void QuicConnectionPImpl::prepare_stream_write(QuicStreamID sid, bool padding, StreamWriteContext& ctx,
                                               std::vector<ngtcp2_vec>& datav) {
  ctx = StreamWriteContext{};
  if (padding) {
    ctx.flags |= NGTCP2_WRITE_STREAM_FLAG_PADDING;
  }
  datav.clear();

  if (sid == -1) {
    return;
  }

  auto it = streams_.find(sid);
  CHECK(it != streams_.end());

  auto& st = it->second;

  ctx.unsent_before = st.reader_.size();
  build_unsent_vecs(datav, st);

  if (st.fin_pending) {
    ctx.flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
  }
}

void QuicConnectionPImpl::finish_stream_write(QuicStreamID sid, const StreamWriteContext& ctx, ngtcp2_ssize pdatalen) {
  if (sid == -1) {
    return;
  }
  auto it = streams_.find(sid);
  if (it == streams_.end()) {
    return;
  }
  auto& st = it->second;

  if (pdatalen > 0) {
    st.reader_.advance(pdatalen);
  }

  if ((ctx.flags & NGTCP2_WRITE_STREAM_FLAG_FIN) != 0 && pdatalen >= 0) {
    if (static_cast<uint64_t>(pdatalen) == ctx.unsent_before) {
      st.fin_pending = false;
      st.fin_submitted = true;
    }
  }
}

void QuicConnectionPImpl::start_batch() {
  CHECK(write_sid_ == -1);
  write_sid_ = pop_ready_stream();
}
QuicStreamID QuicConnectionPImpl::next_ready_stream_id() {
  while (write_sid_ != -1) {
    auto it = streams_.find(write_sid_);
    if (it != streams_.end() && is_stream_ready(it->second)) {
      break;
    }
    if (it != streams_.end()) {
      try_enqueue_stream(write_sid_);
    }
    write_sid_ = pop_ready_stream();
  }

  return write_sid_;
}
void QuicConnectionPImpl::finish_batch() {
  try_enqueue_stream(write_sid_);
  write_sid_ = -1;
}

ngtcp2_ssize QuicConnectionPImpl::write_streams_to_packet(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest,
                                                          size_t destlen, bool padding, ngtcp2_tstamp ts) {
  ngtcp2_ssize n_write = 0;
  size_t streams_in_packet = 0;

  for (;;) {
    auto sid = next_ready_stream_id();
    StreamWriteContext ctx;
    prepare_stream_write(sid, padding, ctx, write_datav_);

    ctx.flags |= NGTCP2_WRITE_STREAM_FLAG_MORE;
    ngtcp2_ssize pdatalen = -1;
    n_write =
        ngtcp2_conn_writev_stream(conn(), path, pi, dest, destlen, sid == -1 ? nullptr : &pdatalen, ctx.flags, sid,
                                  write_datav_.empty() ? nullptr : write_datav_.data(), write_datav_.size(), ts);

    finish_stream_write(sid, ctx, pdatalen);

    if (pdatalen > 0) {
      ++streams_in_packet;
    }

    if (n_write == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
      streams_[sid].is_blocked = true;
      continue;
    }
    if (n_write == NGTCP2_ERR_STREAM_SHUT_WR) {
      streams_[sid].is_write_closed = true;
      continue;
    }
    if (n_write != NGTCP2_ERR_WRITE_MORE) {
      break;
    }
  }

  last_packet_streams_ = streams_in_packet;
  return n_write;
}

ngtcp2_ssize QuicConnectionPImpl::write_pkt_cb(ngtcp2_conn* /*conn*/, ngtcp2_path* path, ngtcp2_pkt_info* pi,
                                               uint8_t* dest, size_t destlen, ngtcp2_tstamp ts, void* user_data) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  return pimpl->write_pkt_aggregate(path, pi, dest, destlen, ts);
}

ngtcp2_ssize QuicConnectionPImpl::write_pkt_aggregate(ngtcp2_path* path, ngtcp2_pkt_info* pi, uint8_t* dest,
                                                      size_t destlen, ngtcp2_tstamp ts) {
  return write_streams_to_packet(path, pi, dest, destlen, true, ts);
}

td::Status QuicConnectionPImpl::produce_egress(UdpMessageBuffer& msg_out, bool use_gso, size_t max_packets) {
  td::PerfWarningTimer w("produce_egress", 0.1);

  const auto ts = now_ts();
  auto path = make_path();
  ngtcp2_pkt_info pi{};
  size_t gso_size = 0;
  ngtcp2_ssize n_write = -1;

  start_batch();
  if (use_gso) {
    n_write = ngtcp2_conn_write_aggregate_pkt2(conn(), &path, &pi, reinterpret_cast<uint8_t*>(msg_out.storage.data()),
                                               msg_out.storage.size(), &gso_size, &write_pkt_cb, max_packets, ts);
  } else {
    n_write = write_streams_to_packet(&path, &pi, reinterpret_cast<uint8_t*>(msg_out.storage.data()),
                                      msg_out.storage.size(), false, ts);
  }
  finish_batch();

  if (n_write < 0) {
    return td::Status::Error(PSTRING() << "ngtcp2_conn_write_aggregate_pkt2 failed: " << n_write);
  }

  ngtcp2_conn_update_pkt_tx_time(conn(), ts);
  commit_write(msg_out, static_cast<size_t>(n_write), gso_size);

  return td::Status::OK();
}

td::Status QuicConnectionPImpl::handle_ingress(const UdpMessageBuffer& msg_in) {
  ngtcp2_path path = make_path();
  ngtcp2_pkt_info pi{};
  int rv = ngtcp2_conn_read_pkt(conn(), &path, &pi, reinterpret_cast<uint8_t*>(msg_in.storage.data()),
                                msg_in.storage.size(), now_ts());
  if (rv == 0) {
    return td::Status::OK();
  }
  if (rv == NGTCP2_ERR_DROP_CONN || ngtcp2_err_is_fatal(rv)) {
    return td::Status::Error(PSTRING() << "ngtcp2_conn_read_pkt failed: " << rv);
  }
  return td::Status::OK();
}

ngtcp2_conn_info QuicConnectionPImpl::get_conn_info() const {
  ngtcp2_conn_info info{};
  ngtcp2_conn_get_conn_info(conn(), &info);
  return info;
}

QuicConnectionId QuicConnectionPImpl::get_primary_scid() const {
  return primary_scid_;
}

void QuicConnectionPImpl::shutdown_stream(QuicStreamID sid) {
  ngtcp2_conn_shutdown_stream(conn(), 0, sid, 1);
}

td::Result<QuicStreamID> QuicConnectionPImpl::open_stream() {
  QuicStreamID sid;

  int rv = ngtcp2_conn_open_bidi_stream(conn(), &sid, nullptr);
  if (rv != 0) {
    return td::Status::Error(PSTRING() << "ngtcp2_conn_open_bidi_stream failed: " << rv);
  }

  CHECK(streams_.emplace(sid, OutboundStreamState{}).second);
  return sid;
}

td::Status QuicConnectionPImpl::buffer_stream(QuicStreamID sid, td::BufferSlice data, bool fin) {
  auto it = streams_.find(sid);
  if (it == streams_.end()) {
    return td::Status::Error("stream not opened");
  }
  auto& st = it->second;
  if (st.fin_pending || st.fin_submitted) {
    return td::Status::Error("stream already closed");
  }
  st.writer_.append(std::move(data));
  st.reader_.sync_with_writer();
  st.pin_.sync_with_writer();
  if (fin) {
    st.fin_pending = true;
  }
  mark_stream_ready(sid, st);
  return td::Status::OK();
}

ngtcp2_tstamp QuicConnectionPImpl::now_ts() {
  return to_ngtcp2_tstamp(td::Timestamp::now());
}

td::Timestamp QuicConnectionPImpl::get_expiry_timestamp() const {
  return from_ngtcp2_tstamp(ngtcp2_conn_get_expiry(conn()));
}

bool QuicConnectionPImpl::is_expired() const {
  auto expiry = ngtcp2_conn_get_expiry(conn());
  return expiry != NGTCP2_TSTAMP_INF && expiry <= now_ts();
}

td::Result<QuicConnectionPImpl::ExpiryAction> QuicConnectionPImpl::handle_expiry() {
  int rv = ngtcp2_conn_handle_expiry(conn(), now_ts());

  if (rv == 0) {
    return ExpiryAction::ScheduleWrite;
  }

  if (rv == NGTCP2_ERR_IDLE_CLOSE) {
    return ExpiryAction::IdleClose;
  }

  return ExpiryAction::Close;
}

ngtcp2_conn* QuicConnectionPImpl::get_pimpl_from_ref(ngtcp2_crypto_conn_ref* ref) {
  auto* c = static_cast<QuicConnectionPImpl*>(ref->user_data);
  return c->conn();
}

int QuicConnectionPImpl::on_handshake_completed() {
  Callback::HandshakeCompletedEvent event;
  // Extract peer's Ed25519 public key from RPK (if available)
  if (EVP_PKEY* peer_rpk = SSL_get0_peer_rpk(ssl_.get())) {
    if (EVP_PKEY_id(peer_rpk) == EVP_PKEY_ED25519) {
      size_t len = td::Ed25519::PublicKey::LENGTH;
      td::SecureString key(len);
      if (EVP_PKEY_get_raw_public_key(peer_rpk, key.as_mutable_slice().ubegin(), &len) == 1 &&
          len == td::Ed25519::PublicKey::LENGTH) {
        event.peer_public_key = std::move(key);
      }
    }
  }

  callback_->on_handshake_completed(std::move(event));
  return 0;
}

int QuicConnectionPImpl::on_recv_stream_data(uint32_t flags, int64_t stream_id, td::Slice data) {
  Callback::StreamDataEvent event{
      .sid = stream_id, .data = td::BufferSlice{data}, .fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0};

  ngtcp2_conn_extend_max_stream_offset(conn(), stream_id, data.size());
  ngtcp2_conn_extend_max_offset(conn(), data.size());

  auto status = callback_->on_stream_data(std::move(event));
  if (status.is_error()) {
    shutdown_stream(stream_id);
    return 0;
  }

  // bidi stream initiated by other party
  if (ngtcp2_is_bidi_stream(stream_id) && !ngtcp2_conn_is_local_stream(conn(), stream_id)) {
    // allow to write into this stream
    streams_.emplace(stream_id, OutboundStreamState{});
  }

  return 0;
}

int QuicConnectionPImpl::on_acked_stream_data_offset(int64_t stream_id, uint64_t offset, uint64_t datalen) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return 0;
  }
  auto& st = it->second;

  LOG_CHECK(offset == st.acked_prefix) << "acked_stream_data_offset gap for stream " << stream_id << ": got " << offset
                                       << " expected " << st.acked_prefix;
  st.acked_prefix = offset + datalen;
  st.pin_.advance(datalen);

  if (datalen == 0) {
    CHECK(st.fin_submitted);
    st.fin_acked = true;
  }

  return 0;
}

int QuicConnectionPImpl::on_stream_close(int64_t stream_id) {
  streams_.erase(stream_id);
  ngtcp2_conn_extend_max_streams_bidi(conn(), 1);
  callback_->on_stream_closed(stream_id);
  return 0;
}

int QuicConnectionPImpl::on_extend_max_stream_data(QuicStreamID sid, uint64_t max_data) {
  auto it = streams_.find(sid);
  if (it == streams_.end()) {
    return 0;
  }
  auto& st = it->second;
  st.is_blocked = false;
  mark_stream_ready(sid, st);
  return 0;
}

void QuicConnectionPImpl::rand_cb(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx) {
  td::Random::secure_bytes(td::MutableSlice(dest, destlen));
}

int QuicConnectionPImpl::get_new_connection_id_cb(ngtcp2_conn* conn, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                                  void* user_data) {
  *cid = QuicConnectionIdAccess::to_ngtcp2(QuicConnectionId::random(cidlen));
  td::Random::secure_bytes(td::MutableSlice(token, NGTCP2_STATELESS_RESET_TOKENLEN));
  return 0;
}

int QuicConnectionPImpl::handshake_completed_cb(ngtcp2_conn* conn, void* user_data) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  return pimpl->on_handshake_completed();
}

int QuicConnectionPImpl::recv_stream_data_cb(ngtcp2_conn* conn, uint32_t flags, int64_t stream_id, uint64_t offset,
                                             const uint8_t* data, size_t datalen, void* user_data,
                                             void* stream_user_data) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  return pimpl->on_recv_stream_data(flags, stream_id, datalen != 0 ? td::Slice(data, datalen) : td::Slice());
}

int QuicConnectionPImpl::acked_stream_data_offset_cb(ngtcp2_conn*, int64_t stream_id, uint64_t offset, uint64_t datalen,
                                                     void* user_data, void* /*stream_user_data*/) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  return pimpl->on_acked_stream_data_offset(stream_id, offset, datalen);
}

int QuicConnectionPImpl::stream_close_cb(ngtcp2_conn*, uint32_t /*flags*/, int64_t stream_id,
                                         uint64_t /*app_error_code*/, void* user_data, void* /*stream_user_data*/) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  return pimpl->on_stream_close(stream_id);
}

int QuicConnectionPImpl::extend_max_stream_data_cb(ngtcp2_conn*, int64_t stream_id, uint64_t max_data, void* user_data,
                                                   void* /*stream_user_data*/) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  pimpl->on_extend_max_stream_data(stream_id, max_data);
  return 0;
}
}  // namespace ton::quic
