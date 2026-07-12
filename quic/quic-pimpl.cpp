#include <algorithm>
#include <cstring>
#include <limits>
#include <openssl/ssl.h>

#include "td/utils/Random.h"
#include "td/utils/Timer.h"
#include "td/utils/logging.h"

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

static void apply_platform_pmtu_policy(ngtcp2_settings& settings) {
  if (td::UdpSocketFd::has_pmtudisc_probe()) {
    return;
  }
  // Without socket-level PMTU probe mode, stay at QUIC's safe minimum and avoid PMTUD growth.
  settings.max_tx_udp_payload_size = NGTCP2_MAX_UDP_PAYLOAD_SIZE;
  settings.no_pmtud = 1;
}

td::Result<std::unique_ptr<QuicConnectionPImpl>> QuicConnectionPImpl::create_client(
    const td::IPAddress& local_address, const td::IPAddress& remote_address, const td::Ed25519::PrivateKey& client_key,
    td::Slice alpn, td::Slice sni, std::unique_ptr<Callback> callback, QuicConnectionOptions options) {
  auto p_impl = std::make_unique<QuicConnectionPImpl>(td::Badge<QuicConnectionPImpl>{}, local_address, remote_address,
                                                      std::move(callback), options);

  TRY_STATUS(p_impl->init_tls_client_rpk(client_key, alpn, sni));
  TRY_STATUS(p_impl->init_quic_client());

  p_impl->callback_->set_connection_id(p_impl->primary_scid_);

  return std::move(p_impl);
}

td::Result<std::unique_ptr<QuicConnectionPImpl>> QuicConnectionPImpl::create_server(
    const td::IPAddress& local_address, const td::IPAddress& remote_address, td::Ref<ServerIdentities> identities,
    td::Slice alpn, const ServerInitialInfo& initial, std::unique_ptr<Callback> callback,
    QuicConnectionOptions options) {
  CHECK(identities.not_null());
  CHECK(identities->has_default());

  auto p_impl = std::make_unique<QuicConnectionPImpl>(td::Badge<QuicConnectionPImpl>{}, local_address, remote_address,
                                                      std::move(callback), options);

  TRY_STATUS(p_impl->init_tls_server_rpk(std::move(identities), alpn));
  TRY_STATUS(p_impl->init_quic_server(initial));

  p_impl->callback_->set_connection_id(p_impl->primary_scid_);

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

using Ed25519EvpKeyPtr = openssl_ptr<EVP_PKEY, &EVP_PKEY_free>;

static td::Result<Ed25519EvpKeyPtr> make_ed25519_evp_key(const td::Ed25519::PrivateKey& key) {
  auto key_bytes = key.as_octet_string();
  OPENSSL_MAKE_PTR(evp_key, EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, key_bytes.as_slice().ubegin(), 32),
                   EVP_PKEY_free, "Failed to create Ed25519 key from raw bytes");
  return std::move(evp_key);
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

  TRY_RESULT(evp_key, make_ed25519_evp_key(key));
  OPENSSL_CHECK_OK(SSL_CTX_use_PrivateKey(ssl_ctx, evp_key.get()), "Failed to set private key");
  return td::Status::OK();
}

static td::Status use_rpk_private_key(SSL* ssl, const td::Ed25519::PrivateKey& key) {
  TRY_RESULT(evp_key, make_ed25519_evp_key(key));
  OPENSSL_CHECK_OK(SSL_use_PrivateKey(ssl, evp_key.get()), "Failed to set private key");
  return td::Status::OK();
}

static int sni_alert(int* ad, int alert, td::Slice reason) {
  // UNRECOGNIZED_NAME is fully attacker-controlled, so keep it out of the warning log to avoid flooding.
  if (alert == SSL_AD_UNRECOGNIZED_NAME) {
    LOG(DEBUG) << "SNI dispatch: " << reason;
  } else {
    LOG(WARNING) << "SNI dispatch: " << reason;
  }
  CHECK(ad != nullptr);
  *ad = alert;
  return SSL_TLSEXT_ERR_ALERT_FATAL;
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

td::Status QuicConnectionPImpl::init_tls_client_rpk(const td::Ed25519::PrivateKey& client_key, td::Slice alpn,
                                                    td::Slice sni) {
  OPENSSL_MAKE_PTR(ssl_ctx_ptr, SSL_CTX_new(TLS_client_method()), SSL_CTX_free, "Failed to create TLS client context");
  TRY_STATUS(setup_rpk_context(ssl_ctx_ptr.get(), client_key));
  setup_alpn_wire(alpn);

  OPENSSL_MAKE_PTR(ssl_ptr, SSL_new(ssl_ctx_ptr.get()), SSL_free, "Failed to create SSL session");
  SSL_set_connect_state(ssl_ptr.get());

  SSL_set_alpn_protos(ssl_ptr.get(), reinterpret_cast<const unsigned char*>(alpn_wire_.c_str()),
                      static_cast<unsigned int>(alpn_wire_.size()));

  if (!sni.empty()) {
    std::string sni_str = sni.str();
    if (SSL_set_tlsext_host_name(ssl_ptr.get(), sni_str.c_str()) != 1) {
      return td::Status::Error("SSL_set_tlsext_host_name failed");
    }
  }

  return finish_tls_setup(std::move(ssl_ptr), std::move(ssl_ctx_ptr), true);
}

bool ServerIdentities::add_identity(ServerIdentity identity) {
  auto sni = identity.sni();
  if (by_sni.contains(sni)) {
    return false;
  }
  auto [it, inserted] = by_sni.emplace(std::move(sni), std::move(identity));
  CHECK(inserted);
  if (default_sni.empty()) {
    default_sni = it->first;
  }
  return true;
}

ServerIdentities* ServerIdentities::make_copy() const {
  auto* copy = new ServerIdentities;
  for (const auto& [sni, identity] : by_sni) {
    copy->by_sni.emplace(sni, ServerIdentity{.local_id = identity.local_id, .key{identity.key.as_octet_string()}});
  }
  copy->default_sni = default_sni;
  return copy;
}

int QuicConnectionPImpl::sni_select_cb(SSL* ssl, int* ad, void* arg) {
  const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (name == nullptr) {
    // No SNI from the client — handshake proceeds with the default key that was installed on the
    // SSL by init_tls_server_rpk.
    return SSL_TLSEXT_ERR_OK;
  }

  // arg is a non-owning pointer to the identity snapshot pinned for the duration of the handshake.
  auto* identities = static_cast<const ServerIdentities*>(arg);
  CHECK(identities != nullptr);

  std::string normalized_name = td::to_lower(td::CSlice(name));
  auto it = identities->by_sni.find(normalized_name);
  if (it == identities->by_sni.end()) {
    return sni_alert(ad, SSL_AD_UNRECOGNIZED_NAME, PSLICE() << "unknown identity " << name);
  }

  if (it->first == identities->default_sni) {
    // SNI resolves to the default identity anyway, so no key swap is needed.
    return SSL_TLSEXT_ERR_OK;
  }

  auto status = use_rpk_private_key(ssl, it->second.key);
  if (status.is_error()) {
    return sni_alert(ad, SSL_AD_INTERNAL_ERROR, PSLICE() << "failed to install key for " << name << ": " << status);
  }
  return SSL_TLSEXT_ERR_OK;
}

td::Status QuicConnectionPImpl::init_tls_server_rpk(td::Ref<ServerIdentities> identities, td::Slice alpn) {
  CHECK(identities.not_null());
  server_identities_ = std::move(identities);

  const auto& default_entry = server_identities_->by_sni.at(server_identities_->default_sni);
  OPENSSL_MAKE_PTR(ssl_ctx_ptr, SSL_CTX_new(TLS_server_method()), SSL_CTX_free, "Failed to create TLS server context");
  TRY_STATUS(setup_rpk_context(ssl_ctx_ptr.get(), default_entry.key));
  setup_alpn_wire(alpn);

  SSL_CTX_set_alpn_select_cb(ssl_ctx_ptr.get(), alpn_select_cb, &alpn_wire_);

  SSL_CTX_set_tlsext_servername_callback(ssl_ctx_ptr.get(), sni_select_cb);
  SSL_CTX_set_tlsext_servername_arg(ssl_ctx_ptr.get(), const_cast<ServerIdentities*>(server_identities_.get()));

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
  settings.handshake_timeout = options.handshake_timeout;  // default disables it; we always set it

  static constexpr ngtcp2_cc_algo CC_ALGO_MAP[] = {NGTCP2_CC_ALGO_CUBIC, NGTCP2_CC_ALGO_RENO, NGTCP2_CC_ALGO_BBR};
  auto cc_alg_id = static_cast<size_t>(options.cc_algo);
  CHECK(cc_alg_id < std::size(CC_ALGO_MAP));
  settings.cc_algo = CC_ALGO_MAP[cc_alg_id];
  apply_platform_pmtu_policy(settings);

  ngtcp2_transport_params_default(&params);
  params.max_idle_timeout = options.idle_timeout;
  params.initial_max_streams_bidi = options.max_streams_bidi;
  params.initial_max_stream_data_bidi_remote = options.initial_max_stream_data_bidi_remote;
  params.initial_max_stream_data_bidi_local = options.initial_max_stream_data_bidi_local;
  params.initial_max_data = options.initial_max_data;
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
  callbacks.remove_connection_id = remove_connection_id_cb;
  callbacks.handshake_completed = handshake_completed_cb;
  callbacks.recv_stream_data = recv_stream_data_cb;
  callbacks.acked_stream_data_offset = acked_stream_data_offset_cb;
  callbacks.stream_close = stream_close_cb;
  callbacks.extend_max_stream_data = extend_max_stream_data_cb;
}

void QuicConnectionPImpl::finish_quic_init(const QuicConnectionId& scid) {
  primary_scid_ = scid;
  ngtcp2_conn_set_tls_native_handle(conn(), ossl_ctx_.get());
  ngtcp2_conn_set_keep_alive_timeout(conn(), options_.keep_alive_timeout);
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
  finish_quic_init(scid);
  return td::Status::OK();
}

td::Status QuicConnectionPImpl::init_quic_server(const ServerInitialInfo& initial) {
  ngtcp2_callbacks callbacks{};
  setup_ngtcp2_callbacks(callbacks, false);

  ngtcp2_settings settings;
  ngtcp2_transport_params params;
  setup_settings_and_params(settings, params, options_);

  params.original_dcid_present = 1;
  params.original_dcid = QuicConnectionIdAccess::to_ngtcp2(initial.original_dcid);
  if (initial.retry_scid.has_value()) {
    params.retry_scid_present = 1;
    params.retry_scid = QuicConnectionIdAccess::to_ngtcp2(*initial.retry_scid);
  }

  auto client_scid = QuicConnectionIdAccess::to_ngtcp2(initial.packet.scid);
  auto server_scid = QuicConnectionId::random();
  auto server_scid_raw = QuicConnectionIdAccess::to_ngtcp2(server_scid);

  ngtcp2_path path = make_path();

  ngtcp2_conn* new_conn = nullptr;
  int rv = ngtcp2_conn_server_new(&new_conn, &client_scid, &server_scid_raw, &path, initial.packet.version, &callbacks,
                                  &settings, &params, nullptr, this);
  if (rv != 0) {
    return td::Status::Error(PSTRING() << "ngtcp2_conn_server_new failed: " << rv);
  }
  conn_.reset(new_conn);
  finish_quic_init(server_scid);
  return td::Status::OK();
}

void QuicConnectionPImpl::build_unsent_vecs(std::vector<ngtcp2_vec>& out, OutboundStreamState& st) {
  out.clear();
  auto it = st.reader_.clone();
  while (!it.empty()) {
    auto head = it.prepare_read();
    out.push_back({.base = const_cast<td::uint8*>(head.ubegin()), .len = head.size()});
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
  return make_path(remote_address_);
}

ngtcp2_path QuicConnectionPImpl::make_path(const td::IPAddress& remote) const {
  return {
      .local = {.addr = const_cast<ngtcp2_sockaddr*>(local_address_.get_sockaddr()),
                .addrlen = static_cast<ngtcp2_socklen>(local_address_.get_sockaddr_len())},
      .remote = {.addr = const_cast<ngtcp2_sockaddr*>(remote.get_sockaddr()),
                 .addrlen = static_cast<ngtcp2_socklen>(remote.get_sockaddr_len())},
      .user_data = nullptr,
  };
}

void QuicConnectionPImpl::commit_write(UdpMessageBuffer& msg_out, size_t n_write, size_t gso_size,
                                       const ngtcp2_path& path) {
  msg_out.storage.truncate(n_write);
  msg_out.address.init_sockaddr(reinterpret_cast<sockaddr*>(path.remote.addr), path.remote.addrlen).ignore();
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
  return write_streams_to_packet(path, pi, dest, destlen, false, ts);
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
  commit_write(msg_out, static_cast<size_t>(n_write), gso_size, path);

  return td::Status::OK();
}

// Writes a terminal CONNECTION_CLOSE for `liberr` into `close_out`, or leaves it empty if ngtcp2 has
// nothing to send (e.g. buffer/amplification limits). Only call for errors that warrant a close.
void QuicConnectionPImpl::write_connection_close(UdpMessageBuffer& close_out, int liberr) {
  ngtcp2_ccerr err{};
  if (liberr == NGTCP2_ERR_CRYPTO) {
    ngtcp2_ccerr_set_tls_alert(&err, ngtcp2_conn_get_tls_alert(conn()), nullptr, 0);
  } else {
    ngtcp2_ccerr_set_liberr(&err, liberr, nullptr, 0);
  }
  write_connection_close(close_out, err);
}

// A deliberate application-level close (eviction, policy shed): carries our protocol's close code
// and a short diagnostic reason instead of a transport error.
void QuicConnectionPImpl::write_application_close(UdpMessageBuffer& close_out, td::uint64 code, td::Slice reason) {
  ngtcp2_ccerr err{};
  ngtcp2_ccerr_set_application_error(&err, code, reinterpret_cast<const uint8_t*>(reason.data()), reason.size());
  write_connection_close(close_out, err);
}

void QuicConnectionPImpl::write_connection_close(UdpMessageBuffer& close_out, const ngtcp2_ccerr& err) {
  auto path = make_path();
  ngtcp2_pkt_info pi{};
  auto n = ngtcp2_conn_write_connection_close(conn(), &path, &pi, reinterpret_cast<uint8_t*>(close_out.storage.data()),
                                              close_out.storage.size(), &err, now_ts());
  if (n > 0) {
    commit_write(close_out, static_cast<size_t>(n), 0, path);
  } else {
    close_out.storage.truncate(0);
  }
}

// On entry `close_out.storage` is a full-capacity buffer; on return it holds a CONNECTION_CLOSE
// datagram to send to msg_in.address, or is empty if there is nothing to send.
td::Status QuicConnectionPImpl::handle_ingress(const UdpMessageBuffer& msg_in, UdpMessageBuffer& close_out) {
  ngtcp2_path path = make_path(msg_in.address);
  ngtcp2_pkt_info pi{};
  int rv = ngtcp2_conn_read_pkt(conn(), &path, &pi, reinterpret_cast<uint8_t*>(msg_in.storage.data()),
                                msg_in.storage.size(), now_ts());
  if (rv == 0) {
    close_out.storage.truncate(0);
    return td::Status::OK();
  }
  // ngtcp2 read_pkt contract: DROP_CONN/RETRY are discarded silently and DRAINING/CLOSING forbid any
  // further packet; every other error (including CRYPTO from a rejected SNI) gets a terminal close.
  if (rv == NGTCP2_ERR_DRAINING) {
    // The peer sent CONNECTION_CLOSE: surface its code and reason (our own notices carry http-style
    // codes) so the close shows up in logs with a cause, not just "draining".
    close_out.storage.truncate(0);
    const ngtcp2_ccerr* ccerr = ngtcp2_conn_get_ccerr(conn());
    auto reason =
        ccerr->reasonlen != 0 ? td::Slice(reinterpret_cast<const char*>(ccerr->reason), ccerr->reasonlen) : td::Slice();
    return td::Status::Error(
        rv, PSTRING() << "closed by peer: " << (ccerr->type == NGTCP2_CCERR_TYPE_APPLICATION ? "app" : "transport")
                      << " code=" << ccerr->error_code << " reason=\"" << reason << "\"");
  }
  if (rv == NGTCP2_ERR_DROP_CONN || rv == NGTCP2_ERR_RETRY || rv == NGTCP2_ERR_CLOSING) {
    close_out.storage.truncate(0);
  } else {
    write_connection_close(close_out, rv);
  }
  // Carry rv as the status code so the caller can classify it (ngtcp2_err_is_fatal) for logging.
  return td::Status::Error(rv, PSTRING() << "ngtcp2_conn_read_pkt failed: " << rv
                                         << " tls_alert=" << (int)ngtcp2_conn_get_tls_alert(conn()));
}

ngtcp2_conn_info QuicConnectionPImpl::get_conn_info() const {
  ngtcp2_conn_info info{};
  ngtcp2_conn_get_conn_info(conn(), &info);
  return info;
}

td::Result<QuicConnectionPImpl::InitialCidState> QuicConnectionPImpl::take_initial_cid_state() {
  LOG_CHECK(!local_cid_callbacks_enabled_) << "Initial CID state already taken";

  td::vector<QuicConnectionId> scids;
  auto num_scids = ngtcp2_conn_get_scid(conn(), nullptr);
  if (num_scids == 0) {
    local_cid_callbacks_enabled_ = true;
    return InitialCidState{.primary_scid = primary_scid_, .scids = {}};
  }

  std::vector<ngtcp2_cid> scids_raw(num_scids);
  CHECK(ngtcp2_conn_get_scid(conn(), scids_raw.data()) == num_scids);

  scids.reserve(scids_raw.size());
  for (const auto& scid : scids_raw) {
    TRY_RESULT(cid, QuicConnectionId::from_raw(scid.data, scid.datalen));
    scids.push_back(cid);
  }

  local_cid_callbacks_enabled_ = true;
  return InitialCidState{.primary_scid = primary_scid_, .scids = std::move(scids)};
}

void QuicConnectionPImpl::shutdown_stream(QuicStreamID sid) {
  ngtcp2_conn_shutdown_stream(conn(), 0, sid, 1);
}

void QuicConnectionPImpl::set_stream_receive_credit_from_max_size(QuicStreamID sid, td::uint64 max_size) {
  td::uint64 target_credit =
      std::clamp<td::uint64>(max_size, options_.initial_max_stream_data_bidi_local, options_.max_stream_window);
  if (target_credit > options_.initial_max_stream_data_bidi_local) {
    ngtcp2_conn_extend_max_stream_offset(conn(), sid, target_credit - options_.initial_max_stream_data_bidi_local);
  }
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
  if (options_.max_outbound_buffered_bytes != 0 &&
      outbound_buffered_ + data.size() > options_.max_outbound_buffered_bytes) {
    if (enforce_outbound_cap_) {
      return td::Status::Error(PSTRING() << "outbound buffer limit exceeded: " << outbound_buffered_ + data.size()
                                         << " > " << options_.max_outbound_buffered_bytes);
    }
    LOG(WARNING) << "outbound buffer over cap on trusted/local connection: " << outbound_buffered_ + data.size()
                 << " > " << options_.max_outbound_buffered_bytes;
  }
  outbound_buffered_ += data.size();
  st.writer_.append(std::move(data));
  st.reader_.sync_with_writer();
  st.pin_.sync_with_writer();
  if (fin) {
    st.fin_pending = true;
  }
  mark_stream_ready(sid, st);
  return td::Status::OK();
}

QuicConnectionStats QuicConnectionPImpl::get_stats() {
  ngtcp2_conn_info info;
  ngtcp2_conn_get_conn_info(conn(), &info);
  size_t bytes_unacked = 0, bytes_unsent = 0;
  for (auto& [_, stream] : streams_) {
    bytes_unacked += stream.pin_.size();
    bytes_unsent += stream.reader_.size();
  }
  return {
      .bytes_rx = info.bytes_recv,
      .bytes_tx = info.bytes_sent,
      .bytes_lost = info.bytes_lost,
      .bytes_unacked = bytes_unacked,
      .bytes_unsent = bytes_unsent,
      .total_sids = sids_encountered,
      .open_sids = streams_.size(),
      .mean_rtt = static_cast<double>(info.smoothed_rtt),
  };
}

ngtcp2_tstamp QuicConnectionPImpl::now_ts() {
  return to_ngtcp2_tstamp(td::Timestamp::now());
}

td::Timestamp QuicConnectionPImpl::get_expiry_timestamp() const {
  return from_ngtcp2_tstamp(ngtcp2_conn_get_expiry(conn()));
}

void QuicConnectionPImpl::start_ingress_shaping(IngressAggregate* aggregate) {
  ingress_shaper_.start(aggregate);
}

void QuicConnectionPImpl::stop_ingress_shaping() {
  if (auto debt = ingress_shaper_.stop()) {
    ngtcp2_conn_extend_max_offset(conn(), debt);  // dump accrued debt: peer gets full credit now
  }
}

td::uint64 QuicConnectionPImpl::grant_ingress(td::Timestamp now, td::uint64 cap) {
  auto grant = ingress_shaper_.take(now, cap);
  if (grant) {
    ngtcp2_conn_extend_max_offset(conn(), grant);
  }
  return grant;
}

td::uint64 QuicConnectionPImpl::ingress_debt() const {
  return ingress_shaper_.debt();
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

td::SecureString QuicConnectionPImpl::extract_peer_ed25519_key() const {
  EVP_PKEY* peer_rpk = SSL_get0_peer_rpk(ssl_.get());
  if (!peer_rpk || EVP_PKEY_id(peer_rpk) != EVP_PKEY_ED25519) {
    return {};
  }
  size_t len = td::Ed25519::PublicKey::LENGTH;
  td::SecureString key(len);
  if (EVP_PKEY_get_raw_public_key(peer_rpk, key.as_mutable_slice().ubegin(), &len) != 1 ||
      len != td::Ed25519::PublicKey::LENGTH) {
    return {};
  }
  return key;
}

td::SecureString QuicConnectionPImpl::extract_local_ed25519_key() const {
  // For the server side this reflects the post-SNI-dispatch key actually used in the handshake.
  // For the client side this is just the client's RPK.
  EVP_PKEY* local_pkey = SSL_get_privatekey(ssl_.get());
  if (!local_pkey || EVP_PKEY_id(local_pkey) != EVP_PKEY_ED25519) {
    return {};
  }
  size_t len = td::Ed25519::PublicKey::LENGTH;
  td::SecureString key(len);
  if (EVP_PKEY_get_raw_public_key(local_pkey, key.as_mutable_slice().ubegin(), &len) != 1 ||
      len != td::Ed25519::PublicKey::LENGTH) {
    return {};
  }
  return key;
}

int QuicConnectionPImpl::on_handshake_completed() {
  callback_->on_handshake_completed({
      .peer_public_key = extract_peer_ed25519_key(),
      .local_public_key = extract_local_ed25519_key(),
  });
  if (ssl_ctx_) {
    // Paranoia: clear OpenSSL's server_identities pointer to not potentially access freed memory.
    SSL_CTX_set_tlsext_servername_arg(ssl_ctx_.get(), nullptr);
  }
  server_identities_.clear();
  return 0;
}

int QuicConnectionPImpl::on_recv_stream_data(uint32_t flags, int64_t stream_id, td::Slice data) {
  Callback::StreamDataEvent event{
      .sid = stream_id, .data = td::BufferSlice{data}, .fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0};

  // Shaped connections accrue debt; the server grants connection-window credit from the shared
  // bucket after ingress (QuicServer::service_shaper). Unshaped ones extend at line rate.
  // Per-stream credit (below) is never shaped.
  if (ingress_shaper_.shaped()) {
    ingress_shaper_.on_data(data.size());
  } else {
    ngtcp2_conn_extend_max_offset(conn(), data.size());
  }

  auto status = callback_->on_stream_data(std::move(event));
  if (status.is_error()) {
    shutdown_stream(stream_id);
    return 0;
  }

  ngtcp2_conn_extend_max_stream_offset(conn(), stream_id, data.size());

  // bidi stream initiated by other party
  if (ngtcp2_is_bidi_stream(stream_id) && !ngtcp2_conn_is_local_stream(conn(), stream_id)) {
    // allow to write into this stream
    streams_.emplace(stream_id, OutboundStreamState{});
    sids_encountered++;
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
  CHECK(outbound_buffered_ >= datalen);
  outbound_buffered_ -= datalen;

  if (datalen == 0) {
    CHECK(st.fin_submitted);
    st.fin_acked = true;
  }

  return 0;
}

int QuicConnectionPImpl::on_stream_close(int64_t stream_id) {
  if (auto it = streams_.find(stream_id); it != streams_.end()) {
    CHECK(outbound_buffered_ >= it->second.pin_.size());
    outbound_buffered_ -= it->second.pin_.size();
    streams_.erase(it);
  }
  if (ngtcp2_is_bidi_stream(stream_id) && !ngtcp2_conn_is_local_stream(conn(), stream_id)) {
    ngtcp2_conn_extend_max_streams_bidi(conn(), 1);
  }
  callback_->on_stream_closed(stream_id);
  return 0;
}

int QuicConnectionPImpl::on_extend_max_stream_data(QuicStreamID sid) {
  auto it = streams_.find(sid);
  if (it == streams_.end()) {
    return 0;
  }
  auto& st = it->second;
  st.is_blocked = false;
  mark_stream_ready(sid, st);
  return 0;
}

int QuicConnectionPImpl::on_get_new_connection_id(ngtcp2_cid* cid, uint8_t* token, size_t cidlen) {
  QuicConnectionId new_cid = QuicConnectionId::random(cidlen);
  *cid = QuicConnectionIdAccess::to_ngtcp2(new_cid);
  if (local_cid_callbacks_enabled_) {
    callback_->on_local_cid_issued(new_cid);
  }
  td::Random::secure_bytes(td::MutableSlice(token, NGTCP2_STATELESS_RESET_TOKENLEN));
  return 0;
}

int QuicConnectionPImpl::on_remove_connection_id(const ngtcp2_cid* cid) {
  auto cid_r = QuicConnectionId::from_raw(cid->data, cid->datalen);
  if (cid_r.is_error()) {
    LOG_EVERY(ERROR) << "remove_connection_id received invalid cid";
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  if (local_cid_callbacks_enabled_) {
    callback_->on_local_cid_retired(cid_r.move_as_ok());
  }
  return 0;
}

void QuicConnectionPImpl::rand_cb(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx) {
  td::Random::secure_bytes(td::MutableSlice(dest, destlen));
}

int QuicConnectionPImpl::get_new_connection_id_cb(ngtcp2_conn* /*conn*/, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                                  void* user_data) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  return pimpl->on_get_new_connection_id(cid, token, cidlen);
}

int QuicConnectionPImpl::remove_connection_id_cb(ngtcp2_conn* /*conn*/, const ngtcp2_cid* cid, void* user_data) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  return pimpl->on_remove_connection_id(cid);
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

int QuicConnectionPImpl::extend_max_stream_data_cb(ngtcp2_conn*, int64_t stream_id, uint64_t /*max_data*/,
                                                   void* user_data, void* /*stream_user_data*/) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  pimpl->on_extend_max_stream_data(stream_id);
  return 0;
}
}  // namespace ton::quic
