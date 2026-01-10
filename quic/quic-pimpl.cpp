#include <algorithm>
#include <cstring>
#include <limits>
#include <openssl/ssl.h>
#include <quic-pimpl.h>
#include <vector>

#include "td/utils/Random.h"
#include "td/utils/logging.h"
#include "td/utils/port/UdpSocketFd.h"

#include "openssl-utils.h"
#include "quic-common.h"

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

static ngtcp2_cid gen_cid() {
  ngtcp2_cid cid{};
  cid.datalen = QuicConnectionPImpl::CID_LENGTH;
  td::Random::secure_bytes(td::MutableSlice(cid.data, cid.datalen));
  return cid;
}

static td::Result<ngtcp2_cid> make_cid(const uint8_t* data, size_t len) {
  if (len > NGTCP2_MAX_CIDLEN) {
    return td::Status::Error("CID length exceeds NGTCP2_MAX_CIDLEN");
  }
  ngtcp2_cid cid{};
  ngtcp2_cid_init(&cid, data, len);
  return cid;
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
}

td::Status QuicConnectionPImpl::init_quic_client() {
  ngtcp2_callbacks callbacks{};
  setup_ngtcp2_callbacks(callbacks, true);

  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  settings.initial_ts = now_ts();

  ngtcp2_transport_params params;
  ngtcp2_transport_params_default(&params);

  params.initial_max_streams_bidi = 0;
  params.initial_max_stream_data_bidi_local = DEFAULT_WINDOW;
  params.initial_max_stream_data_bidi_remote = 0;
  params.initial_max_data = DEFAULT_WINDOW;

  ngtcp2_cid dcid = gen_cid();
  ngtcp2_cid scid = gen_cid();

  ngtcp2_path path = make_path();

  ngtcp2_conn* conn = nullptr;
  int rv = ngtcp2_conn_client_new(&conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &callbacks, &settings, &params,
                                  nullptr, this);

  if (rv != 0) {
    return td::Status::Error("ngtcp2_conn_client_new failed");
  }
  conn_.reset(conn);

  ngtcp2_conn_set_tls_native_handle(conn_.get(), ossl_ctx_.get());

  return td::Status::OK();
}

td::Status QuicConnectionPImpl::init_quic_server(const ngtcp2_version_cid& vc) {
  ngtcp2_callbacks callbacks{};
  setup_ngtcp2_callbacks(callbacks, false);

  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  settings.initial_ts = now_ts();

  ngtcp2_transport_params params;
  ngtcp2_transport_params_default(&params);

  params.initial_max_streams_bidi = DEFAULT_STREAM_LIMIT;
  params.initial_max_stream_data_bidi_local = 0;
  params.initial_max_stream_data_bidi_remote = DEFAULT_WINDOW;
  params.initial_max_data = DEFAULT_WINDOW;

  params.original_dcid_present = 1;
  TRY_RESULT_ASSIGN(params.original_dcid, make_cid(vc.dcid, vc.dcidlen));

  ngtcp2_cid client_scid;
  TRY_RESULT_ASSIGN(client_scid, make_cid(vc.scid, vc.scidlen));

  ngtcp2_cid server_scid = gen_cid();

  ngtcp2_path path = make_path();

  ngtcp2_conn* conn = nullptr;
  int rv = ngtcp2_conn_server_new(&conn, &client_scid, &server_scid, &path, vc.version, &callbacks, &settings, &params,
                                  nullptr, this);
  if (rv != 0) {
    return td::Status::Error(PSTRING() << "ngtcp2_conn_server_new failed: " << rv);
  }
  conn_.reset(conn);
  ngtcp2_conn_set_tls_native_handle(conn_.get(), ossl_ctx_.get());
  return td::Status::OK();
}

uint64_t QuicConnectionPImpl::OutboundStreamState::unsent_bytes() const {
  return queued_bytes >= submitted_unacked ? queued_bytes - submitted_unacked : 0;
}

void QuicConnectionPImpl::OutboundStreamState::consume_acked_prefix(uint64_t n) {
  if (n == 0) {
    return;
  }
  if (n > submitted_unacked) {
    n = submitted_unacked;
  }
  submitted_unacked -= n;
  queued_bytes = queued_bytes >= n ? queued_bytes - n : 0;

  while (n > 0 && chunks.size() > 0) {
    auto& bs = chunks.front();
    auto sz = bs.size();
    if (sz <= n) {
      n -= sz;
      chunks.pop_front();
      continue;
    }
    bs.confirm_read(n);
    n = 0;
  }

  while (chunks.size() > 0 && chunks.front().empty()) {
    chunks.pop_front();
  }
}

void QuicConnectionPImpl::build_unsent_vecs(std::vector<ngtcp2_vec>& out, OutboundStreamState& st) {
  out.clear();
  uint64_t skip = st.submitted_unacked;
  for (size_t i = 0; i < st.chunks.size(); ++i) {
    auto slice = st.chunks[i].as_slice();
    if (skip > 0) {
      if (slice.size() <= skip) {
        skip -= slice.size();
        continue;
      }
      slice.remove_prefix(skip);
      skip = 0;
    }
    if (slice.empty()) {
      continue;
    }
    ngtcp2_vec v{};
    v.base = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(slice.data()));
    v.len = slice.size();
    out.push_back(v);
  }
}

ngtcp2_path QuicConnectionPImpl::make_path() const {
  ngtcp2_path path{};
  path.local.addr = const_cast<ngtcp2_sockaddr*>(local_address.get_sockaddr());
  path.local.addrlen = static_cast<ngtcp2_socklen>(local_address.get_sockaddr_len());
  path.remote.addr = const_cast<ngtcp2_sockaddr*>(remote_address.get_sockaddr());
  path.remote.addrlen = static_cast<ngtcp2_socklen>(remote_address.get_sockaddr_len());
  return path;
}

td::Status QuicConnectionPImpl::write_one_packet(UdpMessageBuffer& msg_out, QuicStreamID sid) {
  const auto ts = now_ts();

  std::vector<ngtcp2_vec> datav;
  uint32_t flags = 0;
  uint64_t unsent_before = 0;

  if (sid != -1) {
    auto it = outbound_.find(sid);
    if (it == outbound_.end()) {
      msg_out.storage = msg_out.storage.substr(0, 0);
      return td::Status::OK();
    }
    auto& st = it->second;

    unsent_before = st.unsent_bytes();
    build_unsent_vecs(datav, st);

    if (st.fin_pending) {
      flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
    }
  }

  ngtcp2_pkt_info pi{};
  ngtcp2_ssize pdatalen = -1;

  ngtcp2_ssize n_write = ngtcp2_conn_writev_stream(
      conn_.get(), nullptr, &pi, reinterpret_cast<uint8_t*>(msg_out.storage.data()), msg_out.storage.size(),
      sid == -1 ? nullptr : &pdatalen, flags, sid, datav.empty() ? nullptr : datav.data(), datav.size(), ts);

  if (n_write == 0) {
    msg_out.storage = msg_out.storage.substr(0, 0);
    return td::Status::OK();
  }

  if (n_write < 0) {
    if (n_write == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
      msg_out.storage = msg_out.storage.substr(0, 0);
      return td::Status::OK();
    }
    return td::Status::Error(PSTRING() << "ngtcp2_conn_writev_stream failed: " << n_write);
  }

  ngtcp2_conn_update_pkt_tx_time(conn_.get(), ts);

  if (sid != -1) {
    auto& st = outbound_.find(sid)->second;

    if (pdatalen > 0) {
      auto n = static_cast<uint64_t>(pdatalen);
      if (n > st.unsent_bytes()) {
        n = st.unsent_bytes();
      }
      st.submitted_unacked += n;
    }

    if ((flags & NGTCP2_WRITE_STREAM_FLAG_FIN) != 0 && pdatalen >= 0) {
      if (static_cast<uint64_t>(pdatalen) == unsent_before) {
        st.fin_pending = false;
        st.fin_submitted = true;
      }
    }
  }

  msg_out.storage = msg_out.storage.substr(0, n_write);
  msg_out.address = remote_address;

  return td::Status::OK();
}

td::Status QuicConnectionPImpl::produce_egress(UdpMessageBuffer& msg_out) {
  QuicStreamID sid = -1;
  for (auto& it : outbound_) {
    if (it.second.unsent_bytes() > 0 || it.second.fin_pending) {
      sid = it.first;
      break;
    }
  }
  return write_one_packet(msg_out, sid);
}

td::Status QuicConnectionPImpl::handle_ingress(const UdpMessageBuffer& msg_in) {
  ngtcp2_path path = make_path();

  ngtcp2_pkt_info pi{};
  int rv = ngtcp2_conn_read_pkt(conn_.get(), &path, &pi, reinterpret_cast<uint8_t*>(msg_in.storage.data()),
                                msg_in.storage.size(), now_ts());

  if (rv != 0)
    return td::Status::Error(PSTRING() << "ngtcp2_conn_read_pkt failed: " << rv);

  return td::Status::OK();
}

td::Result<QuicStreamID> QuicConnectionPImpl::open_stream() {
  QuicStreamID sid;

  int rv = ngtcp2_conn_open_bidi_stream(conn_.get(), &sid, nullptr);
  if (rv != 0)
    return td::Status::Error(PSTRING() << "ngtcp2_conn_open_bidi_stream failed: " << rv);

  return sid;
}

td::Status QuicConnectionPImpl::write_stream(UdpMessageBuffer& msg_out, QuicStreamID sid, td::BufferSlice data,
                                             bool fin) {
  auto& st = outbound_[sid];

  if (!data.empty()) {
    st.queued_bytes += data.size();
    st.chunks.push_back(std::move(data));
  }
  if (fin) {
    st.fin_pending = true;
  }

  return write_one_packet(msg_out, sid);
}

ngtcp2_tstamp QuicConnectionPImpl::now_ts() {
  return to_ngtcp2_tstamp(td::Timestamp::now());
}

td::Timestamp QuicConnectionPImpl::get_expiry_timestamp() const {
  if (!conn_) {
    return td::Timestamp::never();
  }
  return from_ngtcp2_tstamp(ngtcp2_conn_get_expiry(conn_.get()));
}

bool QuicConnectionPImpl::is_expired() const {
  if (!conn_) {
    return false;
  }
  auto expiry = ngtcp2_conn_get_expiry(conn_.get());
  return expiry != NGTCP2_TSTAMP_INF && expiry <= now_ts();
}

td::Result<QuicConnectionPImpl::ExpiryAction> QuicConnectionPImpl::handle_expiry() {
  if (!conn_) {
    return ExpiryAction::None;
  }
  int rv = ngtcp2_conn_handle_expiry(conn_.get(), now_ts());

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
  return c->conn_.get();
}

void QuicConnectionPImpl::rand_cb(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx) {
  td::Random::secure_bytes(td::MutableSlice(dest, destlen));
}

int QuicConnectionPImpl::get_new_connection_id_cb(ngtcp2_conn* conn, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                                  void* user_data) {
  cid->datalen = cidlen;
  if (RAND_bytes(cid->data, static_cast<int>(cidlen)) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

int QuicConnectionPImpl::handshake_completed_cb(ngtcp2_conn* conn, void* user_data) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  Callback::HandshakeCompletedEvent event;
  // Extract peer's Ed25519 public key from RPK (if available)
  if (EVP_PKEY* peer_rpk = SSL_get0_peer_rpk(pimpl->ssl_.get())) {
    if (EVP_PKEY_id(peer_rpk) == EVP_PKEY_ED25519) {
      size_t len = td::Ed25519::PublicKey::LENGTH;
      td::SecureString key(len);
      if (EVP_PKEY_get_raw_public_key(peer_rpk, key.as_mutable_slice().ubegin(), &len) == 1 &&
          len == td::Ed25519::PublicKey::LENGTH) {
        event.peer_public_key = std::move(key);
      }
    }
  }

  if (auto status = pimpl->callback->on_handshake_completed(std::move(event)); status.is_error()) {
    LOG(WARNING) << "handshake rejected: " << status;
    //FIXME: we should actually close connection
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

int QuicConnectionPImpl::recv_stream_data_cb(ngtcp2_conn* conn, uint32_t flags, int64_t stream_id, uint64_t offset,
                                             const uint8_t* data, size_t datalen, void* user_data,
                                             void* stream_user_data) {
  data = data ? data : static_cast<uint8_t*>(user_data);  // stupid hack to create empty slice
  td::Slice data_slice(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + datalen);
  Callback::StreamDataEvent event{
      .sid = stream_id, .data = td::BufferSlice{data_slice}, .fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0};
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  pimpl->callback->on_stream_data(std::move(event));

  ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
  ngtcp2_conn_extend_max_offset(conn, datalen);

  return 0;
}

int QuicConnectionPImpl::acked_stream_data_offset_cb(ngtcp2_conn*, int64_t stream_id, uint64_t offset, uint64_t datalen,
                                                     void* user_data, void* /*stream_user_data*/) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  auto it = pimpl->outbound_.find(stream_id);
  if (it == pimpl->outbound_.end()) {
    return 0;
  }
  auto& st = it->second;

  const uint64_t end = offset + datalen;
  if (end <= st.acked_prefix) {
    return 0;
  }
  if (offset < st.acked_prefix) {
    datalen = end - st.acked_prefix;
    offset = st.acked_prefix;
  }
  if (offset != st.acked_prefix) {
    LOG(WARNING) << "acked_stream_data_offset gap for stream " << stream_id << ": got " << offset << " expected "
                 << st.acked_prefix;
    return 0;
  }

  st.acked_prefix = end;

  if (datalen > 0) {
    st.consume_acked_prefix(datalen);
  } else if (st.fin_submitted) {
    st.fin_acked = true;
  }

  return 0;
}

int QuicConnectionPImpl::stream_close_cb(ngtcp2_conn*, uint32_t /*flags*/, int64_t stream_id,
                                         uint64_t /*app_error_code*/, void* user_data, void* /*stream_user_data*/) {
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  pimpl->outbound_.erase(stream_id);
  return 0;
}
}  // namespace ton::quic
