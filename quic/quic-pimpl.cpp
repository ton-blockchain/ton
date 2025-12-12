#include <quic-pimpl.h>

namespace ton::quic {
td::Status QuicConnectionPImpl::init_tls(td::Slice host, td::Slice alpn) {
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

td::Status QuicConnectionPImpl::init_quic() {
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

  params.initial_max_streams_bidi = 4;
  params.initial_max_stream_data_bidi_local = 1 << 20;
  params.initial_max_stream_data_bidi_remote = 1 << 20;
  params.initial_max_data = 1 << 20;

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

td::Status QuicConnectionPImpl::flush_egress() {
  while (true) {
    uint8_t out[QUIC_MTU];

    ngtcp2_path path{};
    path.local.addr = const_cast<ngtcp2_sockaddr*>(local_address.get_sockaddr());
    path.local.addrlen = static_cast<ngtcp2_socklen>(local_address.get_sockaddr_len());
    path.remote.addr = const_cast<ngtcp2_sockaddr*>(remote_address.get_sockaddr());
    path.remote.addrlen = static_cast<ngtcp2_socklen>(remote_address.get_sockaddr_len());

    ngtcp2_pkt_info pi{};

    ngtcp2_ssize n_write = ngtcp2_conn_write_pkt(conn, &path, &pi, out, sizeof(out), now_ts());

    if (n_write < 0) {
      if (n_write == NGTCP2_ERR_WRITE_MORE)
        return td::Status::OK();
      return td::Status::Error("ngtcp2_conn_write_pkt failed");
    }
    if (n_write == 0)
      break;

    bool is_sent;
    td::Slice out_slice(reinterpret_cast<const char*>(out), reinterpret_cast<const char*>(out) + n_write);
    td::UdpSocketFd::OutboundMessage msg{.to = &remote_address, .data = out_slice};
    TRY_STATUS(fd.send_message(msg, is_sent));

    if (!is_sent)
      break;
  }

  return td::Status::OK();
}

td::Status QuicConnectionPImpl::handle_ingress() {
  while (true) {
    uint8_t buf[UDP_MTU];

    bool is_received;
    td::MutableSlice buf_slice(reinterpret_cast<char*>(buf), reinterpret_cast<char*>(buf) + UDP_MTU);
    td::UdpSocketFd::InboundMessage msg{.from = &remote_address, .data = buf_slice, .error = nullptr};
    TRY_STATUS(fd.receive_message(msg, is_received));

    if (!is_received)
      break;

    ngtcp2_path path{};
    path.local.addr = const_cast<ngtcp2_sockaddr*>(local_address.get_sockaddr());
    path.local.addrlen = static_cast<ngtcp2_socklen>(local_address.get_sockaddr_len());
    path.remote.addr = const_cast<ngtcp2_sockaddr*>(remote_address.get_sockaddr());
    path.remote.addrlen = static_cast<ngtcp2_socklen>(remote_address.get_sockaddr_len());

    ngtcp2_pkt_info pi{};
    int rv = ngtcp2_conn_read_pkt(conn, &path, &pi, buf, msg.data.size(), now_ts());

    if (rv != 0)
      return td::Status::Error("ngtcp2_conn_read_pkt failed");
  }

  return td::Status::OK();
}

td::Status QuicConnectionPImpl::write_stream(td::Slice data, bool fin) {
  if (out_sid == -1) {
    int rv = ngtcp2_conn_open_bidi_stream(conn, &out_sid, nullptr);
    if (rv != 0)
      return td::Status::Error("ngtcp2_conn_open_bidi_stream failed");
  }

  ngtcp2_vec vec{};
  vec.base = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(data.data()));
  vec.len = data.size();

  uint8_t out[QUIC_MTU];
  ngtcp2_path path{};
  path.local.addr = const_cast<ngtcp2_sockaddr*>(local_address.get_sockaddr());
  path.local.addrlen = static_cast<ngtcp2_socklen>(local_address.get_sockaddr_len());
  path.remote.addr = const_cast<ngtcp2_sockaddr*>(remote_address.get_sockaddr());
  path.remote.addrlen = static_cast<ngtcp2_socklen>(remote_address.get_sockaddr_len());

  ngtcp2_pkt_info pi{};

  ngtcp2_ssize n_write = ngtcp2_conn_writev_stream(conn, &path, &pi, out, sizeof(out), nullptr,
                                                   NGTCP2_WRITE_STREAM_FLAG_FIN, out_sid, &vec, 1, now_ts());

  if (n_write < 0)
    return td::Status::Error("ngtcp2_conn_writev_stream failed");

  bool is_sent;
  td::Slice out_slice(reinterpret_cast<const char*>(out), reinterpret_cast<const char*>(out) + n_write);
  td::UdpSocketFd::OutboundMessage msg{.to = &remote_address, .data = out_slice};
  TRY_STATUS(fd.send_message(msg, is_sent));

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
  td::Slice data_slice(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + datalen);
  Callback::StreamDataEvent event{.data = data_slice, .fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0};
  static_cast<QuicConnectionPImpl*>(user_data)->callback->on_stream_data(event);
  return 0;
}
}  // namespace ton::quic