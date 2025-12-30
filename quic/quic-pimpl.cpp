#include <algorithm>
#include <cstring>
#include <openssl/ssl.h>
#include <quic-pimpl.h>

#include "td/utils/port/UdpSocketFd.h"

#include "quic-common.h"

namespace ton::quic {

td::Status QuicConnectionPImpl::init_tls_client(td::Slice host, td::Slice alpn) {
  ssl_ctx = SSL_CTX_new(TLS_client_method());
  if (!ssl_ctx)
    return td::Status::Error("SSL_CTX_new failed");

  SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);

  ssl = SSL_new(ssl_ctx);
  if (!ssl)
    return td::Status::Error("SSL_new failed");

  SSL_set_connect_state(ssl);

  std::string host_c(host.begin(), host.end());
  SSL_set_tlsext_host_name(ssl, host_c.c_str());

  std::string alpn_data(alpn.size() + 1, '\0');
  alpn_data[0] = static_cast<int8_t>(alpn.size());
  std::copy_n(alpn.data(), alpn.size(), alpn_data.begin() + 1);
  SSL_set_alpn_protos(ssl, reinterpret_cast<const unsigned char*>(alpn_data.c_str()),
                      static_cast<unsigned int>(alpn_data.size()));

  conn_ref.get_conn = get_pimpl_from_ref;
  conn_ref.user_data = this;
  SSL_set_app_data(ssl, &conn_ref);

  if (ngtcp2_crypto_ossl_configure_client_session(ssl) != 0)
    return td::Status::Error("ngtcp2_crypto_ossl_configure_client_session failed");

  if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx, ssl) != 0)
    return td::Status::Error("ngtcp2_crypto_ossl_ctx_new failed");

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

td::Status QuicConnectionPImpl::init_tls_server(td::Slice cert_file, td::Slice key_file, td::Slice alpn) {
  ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (!ssl_ctx) {
    return td::Status::Error("SSL_CTX_new failed");
  }

  SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);

  SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);

  std::string cert(cert_file.begin(), cert_file.end());
  std::string key(key_file.begin(), key_file.end());
  if (SSL_CTX_use_certificate_file(ssl_ctx, cert.c_str(), SSL_FILETYPE_PEM) != 1) {
    return td::Status::Error("SSL_CTX_use_certificate_file failed");
  }
  if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key.c_str(), SSL_FILETYPE_PEM) != 1) {
    return td::Status::Error("SSL_CTX_use_PrivateKey_file failed");
  }

  alpn_wire_.assign(alpn.size() + 1, '\0');
  alpn_wire_[0] = static_cast<char>(alpn.size());
  std::memcpy(alpn_wire_.data() + 1, alpn.data(), alpn.size());

  SSL_CTX_set_alpn_select_cb(ssl_ctx, alpn_select_cb, &alpn_wire_);

  ssl = SSL_new(ssl_ctx);
  if (!ssl) {
    return td::Status::Error("SSL_new failed");
  }
  SSL_set_accept_state(ssl);

  conn_ref.get_conn = get_pimpl_from_ref;
  conn_ref.user_data = this;
  SSL_set_app_data(ssl, &conn_ref);

  if (ngtcp2_crypto_ossl_configure_server_session(ssl) != 0) {
    return td::Status::Error("ngtcp2_crypto_ossl_configure_server_session failed");
  }

  if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx, ssl) != 0) {
    return td::Status::Error("ngtcp2_crypto_ossl_ctx_new failed");
  }

  return td::Status::OK();
}

td::Status QuicConnectionPImpl::init_quic_client() {
  ngtcp2_callbacks callbacks{};
  callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
  callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
  callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
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

  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  settings.initial_ts = now_ts();

  ngtcp2_transport_params params;
  ngtcp2_transport_params_default(&params);

  params.initial_max_streams_bidi = 0;
  params.initial_max_stream_data_bidi_local = DEFAULT_WINDOW;
  params.initial_max_stream_data_bidi_remote = 0;
  params.initial_max_data = DEFAULT_WINDOW;

  ngtcp2_cid dcid{}, scid{};
  dcid.datalen = 8;
  scid.datalen = 8;
  RAND_bytes(dcid.data, static_cast<int>(dcid.datalen));
  RAND_bytes(scid.data, static_cast<int>(scid.datalen));

  ngtcp2_path path{};
  path.local.addr = const_cast<ngtcp2_sockaddr*>(local_address.get_sockaddr());
  path.local.addrlen = static_cast<ngtcp2_socklen>(local_address.get_sockaddr_len());
  path.remote.addr = const_cast<ngtcp2_sockaddr*>(remote_address.get_sockaddr());
  path.remote.addrlen = static_cast<ngtcp2_socklen>(remote_address.get_sockaddr_len());

  int rv = ngtcp2_conn_client_new(&conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &callbacks, &settings, &params,
                                  nullptr, this);

  if (rv != 0) {
    return td::Status::Error("ngtcp2_conn_client_new failed");
  }

  ngtcp2_conn_set_tls_native_handle(conn, ossl_ctx);

  return td::Status::OK();
}

td::Status QuicConnectionPImpl::init_quic_server(const ngtcp2_version_cid& vc) {
  ngtcp2_callbacks callbacks{};
  callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
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
  params.original_dcid.datalen = std::min<size_t>(vc.dcidlen, sizeof(params.original_dcid.data));
  std::memcpy(params.original_dcid.data, vc.dcid, params.original_dcid.datalen);

  ngtcp2_cid client_scid{};
  client_scid.datalen = std::min<size_t>(vc.scidlen, sizeof(client_scid.data));
  std::memcpy(client_scid.data, vc.scid, client_scid.datalen);

  ngtcp2_cid server_scid{};
  server_scid.datalen = 8;
  RAND_bytes(server_scid.data, static_cast<int>(server_scid.datalen));

  ngtcp2_path path{};
  path.local.addr = const_cast<ngtcp2_sockaddr*>(local_address.get_sockaddr());
  path.local.addrlen = static_cast<ngtcp2_socklen>(local_address.get_sockaddr_len());
  path.remote.addr = const_cast<ngtcp2_sockaddr*>(remote_address.get_sockaddr());
  path.remote.addrlen = static_cast<ngtcp2_socklen>(remote_address.get_sockaddr_len());

  int rv = ngtcp2_conn_server_new(&conn, &client_scid, &server_scid, &path, vc.version, &callbacks, &settings, &params,
                                  nullptr, this);
  if (rv != 0) {
    return td::Status::Error(PSTRING() << "ngtcp2_conn_server_new failed: " << rv);
  }
  ngtcp2_conn_set_tls_native_handle(conn, ossl_ctx);
  return td::Status::OK();
}

td::Status QuicConnectionPImpl::produce_egress(UdpMessageBuffer& msg_out) {
  ngtcp2_pkt_info pi{};
  ngtcp2_ssize n_write = ngtcp2_conn_write_pkt(conn, nullptr, &pi, reinterpret_cast<uint8_t*>(msg_out.storage.data()),
                                               msg_out.storage.size(), now_ts());

  if (n_write < 0) {
    return td::Status::Error("ngtcp2_conn_write_pkt failed");
  }

  msg_out.address = remote_address;
  msg_out.storage = msg_out.storage.substr(0, n_write);

  return td::Status::OK();
}

td::Status QuicConnectionPImpl::handle_ingress(const UdpMessageBuffer& msg_in) {
  ngtcp2_path path{};
  path.local.addr = const_cast<ngtcp2_sockaddr*>(local_address.get_sockaddr());
  path.local.addrlen = static_cast<ngtcp2_socklen>(local_address.get_sockaddr_len());
  path.remote.addr = const_cast<ngtcp2_sockaddr*>(remote_address.get_sockaddr());
  path.remote.addrlen = static_cast<ngtcp2_socklen>(remote_address.get_sockaddr_len());

  ngtcp2_pkt_info pi{};
  int rv = ngtcp2_conn_read_pkt(conn, &path, &pi, reinterpret_cast<uint8_t*>(msg_in.storage.data()),
                                msg_in.storage.size(), now_ts());

  if (rv != 0)
    return td::Status::Error(PSTRING() << "ngtcp2_conn_read_pkt failed: " << rv);

  return td::Status::OK();
}

td::Result<QuicStreamID> QuicConnectionPImpl::open_stream() {
  QuicStreamID sid;

  int rv = ngtcp2_conn_open_bidi_stream(conn, &sid, nullptr);
  if (rv != 0)
    return td::Status::Error(PSTRING() << "ngtcp2_conn_open_bidi_stream failed: " << rv);

  return sid;
}

td::Status QuicConnectionPImpl::write_stream(UdpMessageBuffer& msg_out, QuicStreamID sid, td::Slice data, bool fin) {
  ngtcp2_vec vec{};
  vec.base = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(data.data()));
  vec.len = data.size();

  ngtcp2_pkt_info pi{};
  ngtcp2_ssize n_write = ngtcp2_conn_writev_stream(
      conn, nullptr, &pi, reinterpret_cast<uint8_t*>(msg_out.storage.data()), msg_out.storage.size(), nullptr,
      fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0, sid, &vec, 1, now_ts());

  if (n_write < 0)
    return td::Status::Error(PSTRING() << "ngtcp2_conn_writev_stream failed: " << n_write);

  msg_out.storage = msg_out.storage.substr(0, n_write);
  msg_out.address = remote_address;

  return td::Status::OK();
}

ngtcp2_tstamp QuicConnectionPImpl::now_ts() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

ngtcp2_conn* QuicConnectionPImpl::get_pimpl_from_ref(ngtcp2_crypto_conn_ref* ref) {
  auto* c = static_cast<QuicConnectionPImpl*>(ref->user_data);
  return c->conn;
}

void QuicConnectionPImpl::rand_cb(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx) {
  if (destlen == 0)
    return;
  RAND_bytes(dest, static_cast<int>(destlen));
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
  static_cast<QuicConnectionPImpl*>(user_data)->callback->on_handshake_completed({});
  return 0;
}

int QuicConnectionPImpl::recv_stream_data_cb(ngtcp2_conn*, uint32_t flags, int64_t stream_id, uint64_t offset,
                                             const uint8_t* data, size_t datalen, void* user_data,
                                             void* stream_user_data) {
  data = data ? data : static_cast<uint8_t *>(user_data); // stupid hack to create empty slice
  td::Slice data_slice(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + datalen);
  Callback::StreamDataEvent event{.sid = stream_id, .data = td::BufferSlice{data_slice}, .fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0};
  auto* pimpl = static_cast<QuicConnectionPImpl*>(user_data);
  pimpl->callback->on_stream_data(std::move(event));
  return 0;
}
}  // namespace ton::quic