#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

static ngtcp2_tstamp now_ts() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Client;

static void rand_cb(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* /*rand_ctx*/) {
  if (destlen == 0)
    return;
  RAND_bytes(dest, static_cast<int>(destlen));
}

static int get_new_connection_id_cb(ngtcp2_conn* /*conn*/, ngtcp2_cid* cid, uint8_t* token, size_t cidlen,
                                    void* /*user_data*/) {
  cid->datalen = cidlen;
  if (RAND_bytes(cid->data, static_cast<int>(cidlen)) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

static int handshake_completed_cb(ngtcp2_conn* /*conn*/, void* /*user_data*/) {
  // You could log here.
  return 0;
}

struct Client {
  int fd = -1;

  ngtcp2_conn* conn = nullptr;

  SSL_CTX* ssl_ctx = nullptr;
  SSL* ssl = nullptr;

  ngtcp2_crypto_ossl_ctx* ossl_ctx = nullptr;
  ngtcp2_crypto_conn_ref conn_ref{};

  sockaddr_storage remote_addr{};
  socklen_t remote_addrlen = 0;
  sockaddr_storage local_addr{};
  socklen_t local_addrlen = 0;

  bool request_sent = false;
  int64_t req_stream_id = -1;

  bool response_got = false;
  int64_t res_stream_id = -1;

  bool closing_started = false;

  std::chrono::steady_clock::time_point close_deadline;
  std::chrono::steady_clock::time_point drain_deadline;

  ~Client() {
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
    if (fd != -1) {
      close(fd);
      fd = -1;
    }
  }
};

static int recv_stream_data_cb(ngtcp2_conn* /*conn*/, uint32_t flags, int64_t stream_id, uint64_t /*offset*/,
                               const uint8_t* data, size_t datalen, void* user_data, void* /*stream_user_data*/) {
  if (datalen) {
    std::cout << std::string_view{reinterpret_cast<const char*>(data), datalen};
    std::cout.flush();
  }
  if (flags & NGTCP2_STREAM_DATA_FLAG_FIN) {
    static_cast<Client*>(user_data)->response_got = true;
    static_cast<Client*>(user_data)->res_stream_id = stream_id;
  }
  return 0;
}

static ngtcp2_conn* get_conn_from_ref(ngtcp2_crypto_conn_ref* ref) {
  auto* c = static_cast<Client*>(ref->user_data);
  return c->conn;
}

static bool set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

static bool resolve_and_connect_udp(Client& c, const std::string& host, const std::string& port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
    return false;
  }

  // Pick the first result.
  c.fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (c.fd == -1) {
    freeaddrinfo(res);
    return false;
  }

  if (!set_nonblocking(c.fd)) {
    freeaddrinfo(res);
    return false;
  }

  if (connect(c.fd, res->ai_addr, res->ai_addrlen) != 0) {
    freeaddrinfo(res);
    return false;
  }

  std::memcpy(&c.remote_addr, res->ai_addr, res->ai_addrlen);
  c.remote_addrlen = res->ai_addrlen;

  c.local_addrlen = sizeof(c.local_addr);
  if (getsockname(c.fd, reinterpret_cast<sockaddr*>(&c.local_addr), &c.local_addrlen) != 0) {
    freeaddrinfo(res);
    return false;
  }

  freeaddrinfo(res);
  return true;
}

static bool setup_tls(Client& c, const std::string& host) {
  c.ssl_ctx = SSL_CTX_new(TLS_client_method());
  if (!c.ssl_ctx)
    return false;

  SSL_CTX_set_verify(c.ssl_ctx, SSL_VERIFY_NONE, nullptr);

  c.ssl = SSL_new(c.ssl_ctx);
  if (!c.ssl)
    return false;

  SSL_set_connect_state(c.ssl);

  SSL_set_tlsext_host_name(c.ssl, host.c_str());

  static const unsigned char alpn[] = {0x0a, 'h', 'q', '-', 'i', 'n', 't', 'e', 'r', 'o', 'p'};
  SSL_set_alpn_protos(c.ssl, alpn, sizeof(alpn));

  c.conn_ref.get_conn = get_conn_from_ref;
  c.conn_ref.user_data = &c;
  SSL_set_app_data(c.ssl, &c.conn_ref);

  if (ngtcp2_crypto_ossl_configure_client_session(c.ssl) != 0) {
    return false;
  }

  if (ngtcp2_crypto_ossl_ctx_new(&c.ossl_ctx, c.ssl) != 0) {
    return false;
  }

  return true;
}

static void dump_ssl_errors() {
  unsigned long e;
  while ((e = ERR_get_error()) != 0) {
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    std::fprintf(stderr, "OpenSSL: %s\n", buf);
  }
}

static bool setup_quic(Client& c) {
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
  RAND_bytes(dcid.data, dcid.datalen);
  RAND_bytes(scid.data, scid.datalen);

  ngtcp2_path path{};
  path.local.addr = reinterpret_cast<sockaddr*>(&c.local_addr);
  path.local.addrlen = c.local_addrlen;
  path.remote.addr = reinterpret_cast<sockaddr*>(&c.remote_addr);
  path.remote.addrlen = c.remote_addrlen;

  int rv = ngtcp2_conn_client_new(&c.conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &callbacks, &settings, &params,
                                  /*mem=*/nullptr, /*user_data=*/&c);

  if (rv != 0) {
    std::cerr << "ngtcp2_conn_client_new failed: " << rv << "\n";
    return false;
  }

  ngtcp2_conn_set_tls_native_handle(c.conn, c.ossl_ctx);

  return true;
}

static bool flush_egress(Client& c) {
  if (ngtcp2_conn_in_draining_period(c.conn) || ngtcp2_conn_in_closing_period(c.conn)) {
    return true;
  }

  uint8_t out[1350];
  ngtcp2_path path{};
  path.local.addr = reinterpret_cast<sockaddr*>(&c.local_addr);
  path.local.addrlen = c.local_addrlen;
  path.remote.addr = reinterpret_cast<sockaddr*>(&c.remote_addr);
  path.remote.addrlen = c.remote_addrlen;

  ngtcp2_pkt_info pi{};

  ngtcp2_ssize nwrite = ngtcp2_conn_write_pkt(c.conn, &path, &pi, out, sizeof(out), now_ts());

  if (nwrite < 0) {
    if (nwrite == NGTCP2_ERR_WRITE_MORE) {
      return true;
    }
    return false;
  }
  if (nwrite == 0)
    return true;

  ssize_t sent = send(c.fd, out, static_cast<size_t>(nwrite), 0);
  if (sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return true;
    return false;
  }
  return true;
}

static bool handle_ingress(Client& c) {
  uint8_t buf[2048];

  ssize_t nread = recv(c.fd, buf, sizeof(buf), 0);
  if (nread < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return true;
    return false;
  }
  if (nread == 0)
    return true;

  ngtcp2_path path{};
  path.local.addr = reinterpret_cast<sockaddr*>(&c.local_addr);
  path.local.addrlen = c.local_addrlen;
  path.remote.addr = reinterpret_cast<sockaddr*>(&c.remote_addr);
  path.remote.addrlen = c.remote_addrlen;

  ngtcp2_pkt_info pi{};
  int rv = ngtcp2_conn_read_pkt(c.conn, &path, &pi, buf, static_cast<size_t>(nread), now_ts());

  if (rv != 0) {
    if (rv == NGTCP2_ERR_CLOSING && c.closing_started) {
      return true;
    }
    std::cerr << "ngtcp2_conn_read_pkt error: " << rv << "\n";
    return false;
  }
  return true;
}

static bool send_h09_get_root(Client& c) {
  if (c.request_sent)
    return true;

  int64_t sid = -1;
  int rv = ngtcp2_conn_open_bidi_stream(c.conn, &sid, /*stream_user_data=*/nullptr);
  if (rv != 0) {
    return false;
  }

  c.req_stream_id = sid;

  const char* req = "GET /\r\n";
  ngtcp2_vec vec{};
  vec.base = reinterpret_cast<uint8_t*>(const_cast<char*>(req));
  vec.len = std::strlen(req);

  uint8_t out[1350];
  ngtcp2_path path{};
  path.local.addr = reinterpret_cast<sockaddr*>(&c.local_addr);
  path.local.addrlen = c.local_addrlen;
  path.remote.addr = reinterpret_cast<sockaddr*>(&c.remote_addr);
  path.remote.addrlen = c.remote_addrlen;

  ngtcp2_pkt_info pi{};

  ngtcp2_ssize nwrite =
      ngtcp2_conn_writev_stream(c.conn, &path, &pi, out, sizeof(out),
                                /*pdatalen=*/nullptr, NGTCP2_WRITE_STREAM_FLAG_FIN, sid, &vec, 1, now_ts());

  if (nwrite < 0) {
    std::cerr << "writev_stream failed: " << nwrite << "\n";
    return false;
  }

  if (nwrite > 0) {
    send(c.fd, out, static_cast<size_t>(nwrite), 0);
  }

  c.request_sent = true;
  return true;
}

static bool initiate_close(Client& c) {
  uint8_t out[1350];
  ngtcp2_path path{};
  path.local.addr = reinterpret_cast<sockaddr*>(&c.local_addr);
  path.local.addrlen = c.local_addrlen;
  path.remote.addr = reinterpret_cast<sockaddr*>(&c.remote_addr);
  path.remote.addrlen = c.remote_addrlen;

  ngtcp2_pkt_info pi{};

  ngtcp2_ccerr ccerr;
  ngtcp2_ccerr_default(&ccerr);
  ngtcp2_ccerr_set_application_error(&ccerr, /*error_code=*/0, /*reason=*/nullptr, 0);

  ngtcp2_ssize n = ngtcp2_conn_write_connection_close(c.conn, &path, &pi, out, sizeof(out), &ccerr, now_ts());
  if (n <= 0)
    return false;

  ssize_t sent = send(c.fd, out, static_cast<size_t>(n), 0);
  return sent == n;
}

static int ns_to_ms(uint64_t ns, int clamp_ms = 200) {
  uint64_t ms = ns / 1000000ull;
  if (ms > static_cast<uint64_t>(clamp_ms))
    ms = clamp_ms;
  return static_cast<int>(ms);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <host> <port>\n";
    return 1;
  }

  const std::string host = argv[1];
  const std::string port = argv[2];

  if (ngtcp2_crypto_ossl_init() != 0) {
    std::cerr << "ngtcp2_crypto_ossl_init failed\n";
    return 1;
  }

  Client c;

  if (!resolve_and_connect_udp(c, host, port)) {
    std::cerr << "UDP resolve/connect failed\n";
    return 1;
  }

  if (!setup_tls(c, host)) {
    std::cerr << "TLS setup failed\n";
    return 1;
  }

  if (!setup_quic(c)) {
    std::cerr << "QUIC setup failed\n";
    return 1;
  }

  if (!flush_egress(c)) {
    std::cerr << "Initial write failed\n";
    dump_ssl_errors();
    return 1;
  }

  for (;;) {
    ngtcp2_tstamp now_ns = now_ts();
    ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(c.conn);
    int timeout_ms = 50;
    if (expiry != UINT64_MAX)
      timeout_ms = (expiry <= now_ns) ? 0 : ns_to_ms(expiry - now_ns);

    pollfd pfd{c.fd, POLLIN, 0};
    int pret = poll(&pfd, 1, timeout_ms);
    if (pret < 0) {
      std::perror("poll");
      break;
    }

    if (pret > 0 && (pfd.revents & POLLIN)) {
      if (!handle_ingress(c)) {
        std::cerr << "ingress error\n";
        break;
      }
    }

    ngtcp2_conn_handle_expiry(c.conn, now_ts());

    if (!flush_egress(c)) {
      std::cerr << "egress error\n";
      break;
    }

    if (ngtcp2_conn_get_handshake_completed(c.conn) && !c.request_sent) {
      if (!send_h09_get_root(c)) {
        std::cerr << "send GET failed\n";
        break;
      }
    }

    if (c.response_got && !c.closing_started) {
      if (initiate_close(c)) {
        c.closing_started = true;
        auto now = std::chrono::steady_clock::now();
        c.close_deadline = now + std::chrono::milliseconds(1200);
        c.drain_deadline = now + std::chrono::milliseconds(400);
      }
    }

    if (c.closing_started) {
      bool closing = ngtcp2_conn_in_closing_period(c.conn);
      bool draining = ngtcp2_conn_in_draining_period(c.conn);

      auto now = std::chrono::steady_clock::now();

      if (draining) {
        if (now >= c.drain_deadline)
          break;
        continue;
      }

      if (closing) {
        if (now >= c.close_deadline)
          break;
        continue;
      }

      break;
    }
  }

  return 0;
}