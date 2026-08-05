//! Native Quinn reference driver for the TON transport benchmark shapes.

use std::error::Error;
use std::fmt::Write as _;
use std::io::{self, Write as _};
use std::net::SocketAddr;
use std::str::FromStr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;

use quinn::rustls;
use quinn::{ClientConfig, Connection, Endpoint, ServerConfig, TransportConfig, VarInt};
use rustls::pki_types::{CertificateDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::task::{JoinHandle, JoinSet};
use tokio::time::Instant;

type AnyError = Box<dyn Error + Send + Sync>;

const ALPN: &[u8] = b"ton-quicbench";
const PROBE: &[u8] = b"BP";
const PROBE_RESPONSE: &[u8] = b"P";
const RESPONSE_BYTE: u8 = b'R';

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Role {
    Server,
    Client,
}

impl Role {
    fn as_str(self) -> &'static str {
        match self {
            Self::Server => "server",
            Self::Client => "client",
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Workload {
    Idle,
    Message,
    Query,
    Saturate,
}

impl Workload {
    fn as_str(self) -> &'static str {
        match self {
            Self::Idle => "idle",
            Self::Message => "message",
            Self::Query => "query",
            Self::Saturate => "saturate",
        }
    }
}

impl FromStr for Workload {
    type Err = String;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "idle" => Ok(Self::Idle),
            "message" => Ok(Self::Message),
            "query" => Ok(Self::Query),
            "saturate" => Ok(Self::Saturate),
            _ => Err(format!("unknown workload: {value}")),
        }
    }
}

#[derive(Clone, Debug)]
struct Args {
    role: Role,
    addr: SocketAddr,
    stats_addr: SocketAddr,
    server_addr: SocketAddr,
    servers: usize,
    connections: usize,
    workload: Workload,
    size: usize,
    response_size: usize,
    rate: u64,
    burst: usize,
    inflight: usize,
    connect_inflight: usize,
    connect_rate: f64,
    threads: usize,
    tuned: bool,
    gso: bool,
    duration: Option<Duration>,
}

fn usage() {
    println!(
        "Usage: quicmsgbench-rs --server|--client [options]\n\
         \nCommon:\n\
         \x20 --addr ADDR             QUIC bind/listen address\n\
         \x20 --stats-addr ADDR       HTTP /metrics listen address\n\
         \x20 --workload MODE         idle | message | query (default query)\n\
         \x20 --size N                request/message bytes (default 240)\n\
         \x20 --response-size N       query response bytes (default 240)\n\
         \x20 --threads N             Tokio worker threads (default 1)\n\
         \x20 --tuned                 large credits plus 5s keepalive/15s idle timeout\n\
         \x20 --no-gso                disable Quinn segmentation offload\n\
         \x20 --duration SEC          optional self-termination for smoke tests\n\
         \nClient:\n\
         \x20 --server-addr ADDR      first server address (default 127.0.0.1:29320)\n\
         \x20 --servers N             consecutive destination ports (default 1)\n\
         \x20 --connections N         total connections in this process\n\
         \x20 --rate N                aggregate messages/s (default 4000)\n\
         \x20 --burst N               messages per pacing event (default 1)\n\
         \x20 --inflight N            concurrent queries (default 8)\n\
         \x20 --connect-inflight N    concurrent connection setups (default 256)\n\
         \x20 --connect-rate N        maximum connection starts/s; 0 is unlimited"
    );
}

fn parse_value<T: FromStr>(raw: &[String], index: &mut usize, name: &str) -> Result<T, String>
where
    T::Err: std::fmt::Display,
{
    *index += 1;
    let value = raw
        .get(*index)
        .ok_or_else(|| format!("{name} needs a value"))?;
    value
        .parse()
        .map_err(|e| format!("invalid {name} value {value:?}: {e}"))
}

fn set_role(role: &mut Option<Role>, value: Role) -> Result<(), String> {
    if role.replace(value).is_some() {
        return Err("choose exactly one of --server and --client".into());
    }
    Ok(())
}

fn parse_args() -> Result<Args, String> {
    let raw: Vec<String> = std::env::args().skip(1).collect();
    let mut role = None;
    let mut addr = None;
    let mut stats_addr = None;
    let mut server_addr: SocketAddr = "127.0.0.1:29320".parse().unwrap();
    let mut servers = 1usize;
    let mut connections = None;
    let mut workload = Workload::Query;
    let mut size = 240usize;
    let mut response_size = 240usize;
    let mut rate = 4000u64;
    let mut burst = 1usize;
    let mut inflight = 8usize;
    let mut connect_inflight = 256usize;
    let mut connect_rate = 0.0f64;
    let mut threads = 1usize;
    let mut tuned = false;
    let mut gso = true;
    let mut duration = None;

    let mut i = 0;
    while i < raw.len() {
        match raw[i].as_str() {
            "--server" => set_role(&mut role, Role::Server)?,
            "--client" => set_role(&mut role, Role::Client)?,
            "--addr" => addr = Some(parse_value(&raw, &mut i, "--addr")?),
            "--stats-addr" => stats_addr = Some(parse_value(&raw, &mut i, "--stats-addr")?),
            "--server-addr" => server_addr = parse_value(&raw, &mut i, "--server-addr")?,
            "--servers" => servers = parse_value(&raw, &mut i, "--servers")?,
            "--connections" => connections = Some(parse_value(&raw, &mut i, "--connections")?),
            "--workload" => workload = parse_value(&raw, &mut i, "--workload")?,
            "--size" => size = parse_value(&raw, &mut i, "--size")?,
            "--response-size" => response_size = parse_value(&raw, &mut i, "--response-size")?,
            "--rate" => rate = parse_value(&raw, &mut i, "--rate")?,
            "--burst" => burst = parse_value(&raw, &mut i, "--burst")?,
            "--inflight" => inflight = parse_value(&raw, &mut i, "--inflight")?,
            "--connect-inflight" => {
                connect_inflight = parse_value(&raw, &mut i, "--connect-inflight")?
            }
            "--connect-rate" => connect_rate = parse_value(&raw, &mut i, "--connect-rate")?,
            "--threads" => threads = parse_value(&raw, &mut i, "--threads")?,
            "--tuned" => tuned = true,
            "--no-gso" => gso = false,
            "--duration" | "-d" => {
                let seconds: f64 = parse_value(&raw, &mut i, "--duration")?;
                if !seconds.is_finite() || seconds <= 0.0 {
                    return Err("--duration must be finite and positive".into());
                }
                duration = Some(Duration::from_secs_f64(seconds));
            }
            "--help" | "-h" => {
                usage();
                std::process::exit(0);
            }
            value => return Err(format!("unknown option: {value}")),
        }
        i += 1;
    }

    if workload == Workload::Saturate && role.is_some() {
        return Err("saturate runs both sides in one process; drop --server/--client".into());
    }
    let role = match workload {
        Workload::Saturate => Role::Client,
        _ => role.ok_or("choose --server or --client")?,
    };
    let addr = addr.unwrap_or_else(|| {
        match role {
            Role::Server => "127.0.0.1:29320",
            Role::Client => "127.0.0.1:0",
        }
        .parse()
        .unwrap()
    });
    let stats_addr = stats_addr.unwrap_or_else(|| {
        match role {
            Role::Server => "127.0.0.1:29322",
            Role::Client => "127.0.0.1:29323",
        }
        .parse()
        .unwrap()
    });
    let connections = connections.unwrap_or(servers);

    if servers == 0 || connections == 0 || connections < servers {
        return Err("--connections must be at least --servers, and both must be positive".into());
    }
    if size == 0 || response_size == 0 {
        return Err("--size and --response-size must be positive".into());
    }
    if threads == 0 || inflight == 0 || connect_inflight == 0 || burst == 0 {
        return Err(
            "--threads, --inflight, --connect-inflight and --burst must be positive".into(),
        );
    }
    if workload == Workload::Message && rate == 0 {
        return Err("message workload needs a positive --rate".into());
    }
    if !connect_rate.is_finite() || connect_rate < 0.0 {
        return Err("--connect-rate must be finite and nonnegative".into());
    }
    if u32::from(server_addr.port()) + servers as u32 - 1 > u32::from(u16::MAX) {
        return Err("server port range exceeds 65535".into());
    }

    Ok(Args {
        role,
        addr,
        stats_addr,
        server_addr,
        servers,
        connections,
        workload,
        size,
        response_size,
        rate,
        burst,
        inflight,
        connect_inflight,
        connect_rate,
        threads,
        tuned,
        gso,
        duration,
    })
}

#[derive(Default)]
struct Counters {
    connections_current: AtomicU64,
    connections_total: AtomicU64,
    connections_ready: AtomicU64,
    first_pass_nanoseconds: AtomicU64,
    setup_nanoseconds: AtomicU64,
    setup_reconnects: AtomicU64,
    messages_sent: AtomicU64,
    messages_received: AtomicU64,
    queries_sent: AtomicU64,
    queries_received: AtomicU64,
    queries_completed: AtomicU64,
    bytes_sent: AtomicU64,
    bytes_received: AtomicU64,
    errors: AtomicU64,
}

struct Metrics {
    role: Role,
    workload: Workload,
    tuned: bool,
    gso: bool,
    counters: Arc<Counters>,
    endpoint: Endpoint,
}

impl Metrics {
    fn render(&self) -> String {
        let endpoint = self.endpoint.stats();
        let load = |value: &AtomicU64| value.load(Ordering::Relaxed);
        let mut out = String::new();
        writeln!(
            out,
            "bench_info{{implementation=\"quinn\",role=\"{}\",workload=\"{}\",tuned=\"{}\"}} 1",
            self.role.as_str(),
            self.workload.as_str(),
            self.tuned
        )
        .unwrap();
        writeln!(out, "bench_gso_enabled {}", u8::from(self.gso)).unwrap();
        writeln!(
            out,
            "bench_connections_current {}",
            load(&self.counters.connections_current)
        )
        .unwrap();
        writeln!(
            out,
            "bench_connections_total {}",
            load(&self.counters.connections_total)
        )
        .unwrap();
        writeln!(
            out,
            "bench_connections_ready {}",
            load(&self.counters.connections_ready)
        )
        .unwrap();
        writeln!(
            out,
            "bench_connection_first_pass_seconds {:.9}",
            load(&self.counters.first_pass_nanoseconds) as f64 / 1e9
        )
        .unwrap();
        writeln!(
            out,
            "bench_connection_setup_seconds {:.9}",
            load(&self.counters.setup_nanoseconds) as f64 / 1e9
        )
        .unwrap();
        writeln!(
            out,
            "bench_connection_setup_reconnects_total {}",
            load(&self.counters.setup_reconnects)
        )
        .unwrap();
        writeln!(
            out,
            "bench_messages_sent_total {}",
            load(&self.counters.messages_sent)
        )
        .unwrap();
        writeln!(
            out,
            "bench_messages_received_total {}",
            load(&self.counters.messages_received)
        )
        .unwrap();
        writeln!(
            out,
            "bench_queries_sent_total {}",
            load(&self.counters.queries_sent)
        )
        .unwrap();
        writeln!(
            out,
            "bench_queries_received_total {}",
            load(&self.counters.queries_received)
        )
        .unwrap();
        writeln!(
            out,
            "bench_queries_completed_total {}",
            load(&self.counters.queries_completed)
        )
        .unwrap();
        writeln!(
            out,
            "bench_bytes_sent_total {}",
            load(&self.counters.bytes_sent)
        )
        .unwrap();
        writeln!(
            out,
            "bench_bytes_received_total {}",
            load(&self.counters.bytes_received)
        )
        .unwrap();
        writeln!(out, "bench_errors_total {}", load(&self.counters.errors)).unwrap();
        writeln!(
            out,
            "bench_quic_handshakes_accepted_total {}",
            endpoint.accepted_handshakes
        )
        .unwrap();
        writeln!(
            out,
            "bench_quic_handshakes_outgoing_total {}",
            endpoint.outgoing_handshakes
        )
        .unwrap();
        writeln!(out, "bench_process_cpu_seconds_total {:.6}", cpu_seconds()).unwrap();
        writeln!(out, "bench_process_max_rss_bytes {}", max_rss_bytes()).unwrap();
        out
    }
}

async fn serve_metrics(listener: TcpListener, metrics: Arc<Metrics>) -> io::Result<()> {
    loop {
        let (stream, _) = listener.accept().await?;
        let metrics = metrics.clone();
        tokio::spawn(async move {
            if let Err(error) = serve_metrics_request(stream, metrics).await {
                eprintln!("metrics request failed: {error}");
            }
        });
    }
}

async fn serve_metrics_request(mut stream: TcpStream, metrics: Arc<Metrics>) -> io::Result<()> {
    let mut request = [0u8; 2048];
    let n = stream.read(&mut request).await?;
    let (status, content_type, body) = if request[..n].starts_with(b"GET /metrics ") {
        ("200 OK", "text/plain; version=0.0.4", metrics.render())
    } else {
        ("404 Not Found", "text/plain", "not found\n".to_string())
    };
    let response = format!(
        "HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    stream.write_all(response.as_bytes()).await?;
    stream.shutdown().await
}

#[derive(Debug)]
struct SkipVerify(Arc<rustls::crypto::CryptoProvider>);

impl rustls::client::danger::ServerCertVerifier for SkipVerify {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp: &[u8],
        _now: UnixTime,
    ) -> Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::danger::ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &self.0.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &self.0.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        self.0.signature_verification_algorithms.supported_schemes()
    }
}

fn transport_config(tuned: bool, gso: bool) -> Arc<TransportConfig> {
    let mut config = TransportConfig::default();
    config.enable_segmentation_offload(gso);
    config.datagram_receive_buffer_size(None);
    config.datagram_send_buffer_size(0);
    if tuned {
        config.max_concurrent_uni_streams(VarInt::from_u32(4096));
        config.max_concurrent_bidi_streams(VarInt::from_u32(4096));
        config.stream_receive_window(VarInt::from_u32(4 << 20));
        config.receive_window(VarInt::from_u32(24 << 20));
        config.send_window(24 << 20);
        config.keep_alive_interval(Some(Duration::from_secs(5)));
        config.max_idle_timeout(Some(VarInt::from_u32(15_000).into()));
    }
    Arc::new(config)
}

fn make_server_config(args: &Args) -> Result<ServerConfig, AnyError> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let cert = rcgen::generate_simple_self_signed(vec!["localhost".to_string()])?;
    let cert_der = CertificateDer::from(cert.cert);
    let key_der = PrivatePkcs8KeyDer::from(cert.signing_key.serialize_der());
    let mut crypto = rustls::ServerConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .with_no_client_auth()
        .with_single_cert(vec![cert_der], key_der.into())?;
    crypto.alpn_protocols = vec![ALPN.to_vec()];
    let mut config = ServerConfig::with_crypto(Arc::new(
        quinn::crypto::rustls::QuicServerConfig::try_from(crypto)?,
    ));
    config.transport_config(transport_config(args.tuned, args.gso));
    Ok(config)
}

fn make_client_config(args: &Args) -> Result<ClientConfig, AnyError> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut crypto = rustls::ClientConfig::builder_with_provider(provider.clone())
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(SkipVerify(provider)))
        .with_no_client_auth();
    crypto.alpn_protocols = vec![ALPN.to_vec()];
    let mut config = ClientConfig::new(Arc::new(
        quinn::crypto::rustls::QuicClientConfig::try_from(crypto)?,
    ));
    config.transport_config(transport_config(args.tuned, args.gso));
    Ok(config)
}

async fn run_server(args: Args) -> Result<(), AnyError> {
    let endpoint = Endpoint::server(make_server_config(&args)?, args.addr)?;
    let counters = Arc::new(Counters::default());
    let metrics = Arc::new(Metrics {
        role: args.role,
        workload: args.workload,
        tuned: args.tuned,
        gso: args.gso,
        counters: counters.clone(),
        endpoint: endpoint.clone(),
    });
    let listener = TcpListener::bind(args.stats_addr).await?;
    let stats_addr = listener.local_addr()?;
    let metrics_task = tokio::spawn(serve_metrics(listener, metrics.clone()));
    println!(
        "LISTEN addr={} stats_addr={stats_addr}",
        endpoint.local_addr()?
    );
    io::stdout().flush()?;

    let stop = wait_for_stop(args.duration);
    tokio::pin!(stop);
    loop {
        tokio::select! {
            result = &mut stop => {
                result?;
                break;
            }
            incoming = endpoint.accept() => {
                let Some(incoming) = incoming else { break; };
                let counters = counters.clone();
                let response_size = args.response_size;
                let request_size = args.size;
                tokio::spawn(async move {
                    match incoming.await {
                        Ok(connection) => {
                            counters.connections_total.fetch_add(1, Ordering::Relaxed);
                            counters.connections_current.fetch_add(1, Ordering::Relaxed);
                            handle_server_connection(connection, counters.clone(), request_size, response_size).await;
                            counters.connections_current.fetch_sub(1, Ordering::Relaxed);
                        }
                        Err(_) => { counters.errors.fetch_add(1, Ordering::Relaxed); }
                    }
                });
            }
        }
    }

    print_result(&metrics);
    endpoint.close(VarInt::from_u32(0), b"benchmark shutdown");
    metrics_task.abort();
    Ok(())
}

async fn handle_server_connection(
    connection: Connection,
    counters: Arc<Counters>,
    request_size: usize,
    response_size: usize,
) {
    let ready = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let mut response = vec![0u8; response_size];
    response[0] = RESPONSE_BYTE;
    let response = Arc::new(response);
    loop {
        tokio::select! {
            stream = connection.accept_uni() => {
                let Ok(recv) = stream else { break; };
                let counters = counters.clone();
                tokio::spawn(async move {
                    receive_message(recv, counters, request_size).await;
                });
            }
            stream = connection.accept_bi() => {
                let Ok((send, recv)) = stream else { break; };
                let counters = counters.clone();
                let response = response.clone();
                let ready = ready.clone();
                tokio::spawn(async move {
                    receive_query(send, recv, counters, ready, request_size, response).await;
                });
            }
        }
    }
}

async fn receive_message(mut recv: quinn::RecvStream, counters: Arc<Counters>, size: usize) {
    match recv.read_to_end(size.saturating_add(1)).await {
        Ok(data) if data.len() == size && data.first() == Some(&b'B') => {
            counters.messages_received.fetch_add(1, Ordering::Relaxed);
            counters
                .bytes_received
                .fetch_add(size as u64, Ordering::Relaxed);
        }
        _ => {
            counters.errors.fetch_add(1, Ordering::Relaxed);
        }
    }
}

async fn receive_query(
    mut send: quinn::SendStream,
    mut recv: quinn::RecvStream,
    counters: Arc<Counters>,
    ready: Arc<std::sync::atomic::AtomicBool>,
    size: usize,
    response: Arc<Vec<u8>>,
) {
    let data = match recv
        .read_to_end(size.max(PROBE.len()).saturating_add(1))
        .await
    {
        Ok(data) => data,
        Err(_) => {
            counters.errors.fetch_add(1, Ordering::Relaxed);
            return;
        }
    };
    if data == PROBE {
        if send.write_all(PROBE_RESPONSE).await.is_err() || send.finish().is_err() {
            counters.errors.fetch_add(1, Ordering::Relaxed);
        } else if !ready.swap(true, Ordering::Relaxed) {
            counters.connections_ready.fetch_add(1, Ordering::Relaxed);
        }
        return;
    }
    if data.len() != size || data.first() != Some(&b'B') {
        counters.errors.fetch_add(1, Ordering::Relaxed);
        return;
    }
    counters.queries_received.fetch_add(1, Ordering::Relaxed);
    counters
        .bytes_received
        .fetch_add(size as u64, Ordering::Relaxed);
    if send.write_all(&response).await.is_err() || send.finish().is_err() {
        counters.errors.fetch_add(1, Ordering::Relaxed);
        return;
    }
    counters
        .bytes_sent
        .fetch_add(response.len() as u64, Ordering::Relaxed);
}

async fn run_client(args: Args) -> Result<(), AnyError> {
    let mut endpoint = Endpoint::client(args.addr)?;
    endpoint.set_default_client_config(make_client_config(&args)?);
    let counters = Arc::new(Counters::default());
    let metrics = Arc::new(Metrics {
        role: args.role,
        workload: args.workload,
        tuned: args.tuned,
        gso: args.gso,
        counters: counters.clone(),
        endpoint: endpoint.clone(),
    });
    let listener = TcpListener::bind(args.stats_addr).await?;
    let stats_addr = listener.local_addr()?;
    let metrics_task = tokio::spawn(serve_metrics(listener, metrics.clone()));
    println!(
        "CLIENT addr={} stats_addr={stats_addr}",
        endpoint.local_addr()?
    );
    io::stdout().flush()?;

    let setup_start = Instant::now();
    let connections = connect_all(&args, &endpoint, counters.clone()).await?;
    counters.first_pass_nanoseconds.store(
        setup_start.elapsed().as_nanos().min(u64::MAX as u128) as u64,
        Ordering::Relaxed,
    );
    let settle = if connections.len() >= 100_000 {
        Duration::from_secs(15)
    } else if connections.len() >= 10_000 {
        Duration::from_secs(5)
    } else {
        Duration::from_secs(1)
    };
    tokio::time::sleep(settle).await;
    if connections
        .iter()
        .any(|connection| connection.close_reason().is_some())
    {
        return Err(io::Error::other("a connection closed before readiness stabilized").into());
    }
    counters
        .connections_ready
        .store(connections.len() as u64, Ordering::Relaxed);
    counters.setup_nanoseconds.store(
        setup_start.elapsed().as_nanos().min(u64::MAX as u128) as u64,
        Ordering::Relaxed,
    );
    println!("READY connections={}", connections.len());
    io::stdout().flush()?;

    let workload_args = args.clone();
    let workload_counters = counters.clone();
    let mut workload_task: JoinHandle<Result<(), AnyError>> = tokio::spawn(async move {
        match workload_args.workload {
            // Saturate never reaches run_client; it has its own single-process entry point.
            Workload::Idle | Workload::Saturate => std::future::pending().await,
            Workload::Message => {
                run_messages(
                    connections,
                    workload_counters,
                    workload_args.size,
                    workload_args.rate,
                    workload_args.burst,
                )
                .await
            }
            Workload::Query => {
                run_queries(
                    connections,
                    workload_counters,
                    workload_args.size,
                    workload_args.response_size,
                    workload_args.inflight,
                )
                .await
            }
        }
    });

    tokio::select! {
        result = wait_for_stop(args.duration) => { result?; }
        result = &mut workload_task => {
            result.map_err(|e| io::Error::other(format!("workload task failed: {e}")))??;
            return Err(io::Error::other("workload stopped unexpectedly").into());
        }
    }
    workload_task.abort();
    let _ = workload_task.await;
    print_result(&metrics);
    endpoint.close(VarInt::from_u32(0), b"benchmark shutdown");
    metrics_task.abort();
    Ok(())
}

async fn connect_all(
    args: &Args,
    endpoint: &Endpoint,
    counters: Arc<Counters>,
) -> Result<Arc<Vec<Connection>>, AnyError> {
    let mut pending = JoinSet::new();
    let mut slots: Vec<Option<Connection>> = vec![None; args.connections];
    let started_at = Instant::now();
    let mut next = 0usize;

    while next < args.connections || !pending.is_empty() {
        while next < args.connections && pending.len() < args.connect_inflight {
            if args.connect_rate > 0.0 {
                let deadline =
                    started_at + Duration::from_secs_f64(next as f64 / args.connect_rate);
                tokio::time::sleep_until(deadline).await;
            }
            let index = next;
            next += 1;
            let endpoint = endpoint.clone();
            let mut target = args.server_addr;
            target.set_port(args.server_addr.port() + (index % args.servers) as u16);
            pending.spawn(async move {
                let connection = endpoint.connect(target, "localhost")?.await?;
                readiness_probe(&connection).await?;
                Ok::<_, AnyError>((index, connection))
            });
        }

        let joined = pending
            .join_next()
            .await
            .ok_or_else(|| io::Error::other("connection setup task set became empty"))?;
        match joined {
            Ok(Ok((index, connection))) => {
                counters.connections_total.fetch_add(1, Ordering::Relaxed);
                counters.connections_current.fetch_add(1, Ordering::Relaxed);
                let watched = connection.clone();
                let watched_counters = counters.clone();
                tokio::spawn(async move {
                    watched.closed().await;
                    watched_counters
                        .connections_current
                        .fetch_sub(1, Ordering::Relaxed);
                });
                slots[index] = Some(connection);
            }
            Ok(Err(error)) => {
                counters.errors.fetch_add(1, Ordering::Relaxed);
                return Err(error);
            }
            Err(error) => {
                counters.errors.fetch_add(1, Ordering::Relaxed);
                return Err(
                    io::Error::other(format!("connection setup task failed: {error}")).into(),
                );
            }
        }
    }

    Ok(Arc::new(
        slots
            .into_iter()
            .map(|connection| connection.expect("every setup slot was filled"))
            .collect(),
    ))
}

async fn readiness_probe(connection: &Connection) -> Result<(), AnyError> {
    let (mut send, mut recv) = connection.open_bi().await?;
    send.write_all(PROBE).await?;
    send.finish()?;
    let response = recv.read_to_end(PROBE_RESPONSE.len() + 1).await?;
    if response != PROBE_RESPONSE {
        return Err(io::Error::other("bad readiness response").into());
    }
    Ok(())
}

fn payload(size: usize) -> Arc<Vec<u8>> {
    let mut data = vec![0u8; size];
    data[0] = b'B';
    Arc::new(data)
}

async fn run_messages(
    connections: Arc<Vec<Connection>>,
    counters: Arc<Counters>,
    size: usize,
    rate: u64,
    burst: usize,
) -> Result<(), AnyError> {
    let payload = payload(size);
    let interval = Duration::from_secs_f64(burst as f64 / rate as f64);
    if interval.is_zero() {
        return Err(io::Error::other("message pacing interval rounded to zero").into());
    }
    let mut next_at = Instant::now();
    let mut next_connection = 0usize;
    loop {
        tokio::time::sleep_until(next_at).await;
        for _ in 0..burst {
            let connection = &connections[next_connection % connections.len()];
            next_connection += 1;
            let mut stream = match connection.open_uni().await {
                Ok(stream) => stream,
                Err(error) => {
                    counters.errors.fetch_add(1, Ordering::Relaxed);
                    return Err(error.into());
                }
            };
            if let Err(error) = stream.write_all(&payload).await {
                counters.errors.fetch_add(1, Ordering::Relaxed);
                return Err(error.into());
            }
            if let Err(error) = stream.finish() {
                counters.errors.fetch_add(1, Ordering::Relaxed);
                return Err(error.into());
            }
            counters.messages_sent.fetch_add(1, Ordering::Relaxed);
            counters
                .bytes_sent
                .fetch_add(size as u64, Ordering::Relaxed);
        }
        next_at += interval;
        let now = Instant::now();
        if now.saturating_duration_since(next_at) >= interval {
            next_at = now + interval;
        }
    }
}

async fn run_queries(
    connections: Arc<Vec<Connection>>,
    counters: Arc<Counters>,
    size: usize,
    response_size: usize,
    inflight: usize,
) -> Result<(), AnyError> {
    let payload = payload(size);
    let sequence = Arc::new(AtomicU64::new(0));
    let mut workers = JoinSet::new();
    for _ in 0..inflight {
        let connections = connections.clone();
        let counters = counters.clone();
        let payload = payload.clone();
        let sequence = sequence.clone();
        workers.spawn(async move {
            loop {
                let index = sequence.fetch_add(1, Ordering::Relaxed) as usize % connections.len();
                if let Err(error) =
                    send_query(&connections[index], &payload, response_size, &counters).await
                {
                    counters.errors.fetch_add(1, Ordering::Relaxed);
                    return Err::<(), AnyError>(error);
                }
            }
        });
    }
    match workers.join_next().await {
        Some(Ok(Err(error))) => Err(error),
        Some(Err(error)) => Err(io::Error::other(format!("query worker failed: {error}")).into()),
        _ => Err(io::Error::other("query worker stopped unexpectedly").into()),
    }
}

async fn send_query(
    connection: &Connection,
    payload: &[u8],
    response_size: usize,
    counters: &Counters,
) -> Result<(), AnyError> {
    let (mut send, mut recv) = connection.open_bi().await?;
    send.write_all(payload).await?;
    send.finish()?;
    counters.queries_sent.fetch_add(1, Ordering::Relaxed);
    counters
        .bytes_sent
        .fetch_add(payload.len() as u64, Ordering::Relaxed);
    let response = recv.read_to_end(response_size.saturating_add(1)).await?;
    if response.len() != response_size || response.first() != Some(&RESPONSE_BYTE) {
        return Err(io::Error::other("bad response").into());
    }
    counters.queries_completed.fetch_add(1, Ordering::Relaxed);
    counters
        .bytes_received
        .fetch_add(response.len() as u64, Ordering::Relaxed);
    Ok(())
}

async fn wait_for_stop(duration: Option<Duration>) -> io::Result<()> {
    if let Some(duration) = duration {
        tokio::time::sleep(duration).await;
        Ok(())
    } else {
        std::future::pending().await
    }
}

// Mirrors bench-adnl-quic-message: a single process, one connection, closed-loop with a lag
// window, run to time. A quick one-command ceiling, not a harness workload.
const MAX_RECEIVER_LAG: u64 = 16 * 1024;
const RESUME_RECEIVER_LAG: u64 = MAX_RECEIVER_LAG / 2;

#[derive(Default)]
struct SaturateCounter {
    received: AtomicU64,
    invalid: AtomicU64,
    target: AtomicU64,
    reached: std::sync::Mutex<Option<tokio::sync::oneshot::Sender<()>>>,
}

impl SaturateCounter {
    /// `None` means the target was already reached.
    fn expect(&self, target: u64) -> Option<tokio::sync::oneshot::Receiver<()>> {
        let (tx, rx) = tokio::sync::oneshot::channel();
        let mut guard = self.reached.lock().unwrap();
        if self.received.load(Ordering::Acquire) >= target {
            return None;
        }
        *guard = Some(tx);
        self.target.store(target, Ordering::Release);
        Some(rx)
    }

    fn receive(&self, valid: bool) {
        if !valid {
            self.invalid.fetch_add(1, Ordering::Relaxed);
            return;
        }
        let received = self.received.fetch_add(1, Ordering::Relaxed) + 1;
        let target = self.target.load(Ordering::Acquire);
        if target == 0 || received < target {
            return;
        }
        let mut guard = self.reached.lock().unwrap();
        let target = self.target.load(Ordering::Relaxed);
        if target == 0 || received < target {
            return;
        }
        self.target.store(0, Ordering::Relaxed);
        if let Some(tx) = guard.take() {
            let _ = tx.send(());
        }
    }
}

async fn wait_for(rx: Option<tokio::sync::oneshot::Receiver<()>>) {
    if let Some(rx) = rx {
        let _ = rx.await;
    }
}

async fn run_saturate(args: Args) -> Result<(), AnyError> {
    let server = Endpoint::server(make_server_config(&args)?, "127.0.0.1:0".parse()?)?;
    let server_addr = server.local_addr()?;
    let counter = Arc::new(SaturateCounter::default());
    {
        let counter = counter.clone();
        let size = args.size;
        tokio::spawn(async move {
            while let Some(incoming) = server.accept().await {
                let counter = counter.clone();
                tokio::spawn(async move {
                    let Ok(connection) = incoming.await else {
                        return;
                    };
                    while let Ok(mut recv) = connection.accept_uni().await {
                        let counter = counter.clone();
                        tokio::spawn(async move {
                            match recv.read_to_end(size.saturating_add(1)).await {
                                Ok(data) => counter
                                    .receive(data.len() == size && data.first() == Some(&b'B')),
                                Err(_) => counter.receive(false),
                            }
                        });
                    }
                });
            }
        });
    }

    let mut client = Endpoint::client("127.0.0.1:0".parse()?)?;
    client.set_default_client_config(make_client_config(&args)?);
    let connection = client.connect(server_addr, "localhost")?.await?;

    let payload = payload(args.size);
    let duration = args.duration.unwrap_or(Duration::from_secs(5));

    // Warm up: one message, awaited, so the handshake is outside the measurement.
    let warmup = counter.expect(counter.received.load(Ordering::Relaxed) + 1);
    send_saturate_message(&connection, &payload).await?;
    wait_for(warmup).await;

    let received_at_start = counter.received.load(Ordering::Relaxed);
    let cpu_start = cpu_seconds();
    let mut submitted: u64 = 0;
    let start = Instant::now();
    while start.elapsed() < duration {
        send_saturate_message(&connection, &payload).await?;
        submitted += 1;
        let received = counter.received.load(Ordering::Relaxed) - received_at_start;
        if submitted - submitted.min(received) >= MAX_RECEIVER_LAG {
            wait_for(counter.expect(received_at_start + submitted - RESUME_RECEIVER_LAG)).await;
        }
    }
    let elapsed = start.elapsed().as_secs_f64();
    let received = counter.received.load(Ordering::Relaxed) - received_at_start;

    // Drain everything submitted before sampling CPU, so repeated runs are stable.
    wait_for(counter.expect(received_at_start + submitted)).await;
    let cpu = cpu_seconds() - cpu_start;

    let invalid = counter.invalid.load(Ordering::Relaxed);
    if invalid != 0 {
        return Err(io::Error::other(format!("received {invalid} invalid messages")).into());
    }

    let messages_per_second = received as f64 / elapsed;
    let ns_per_message = if received == 0 {
        0.0
    } else {
        elapsed * 1e9 / received as f64
    };
    let delivery_percent = if submitted == 0 {
        0.0
    } else {
        100.0 * received as f64 / submitted as f64
    };
    let cpu_us_per_message = if received == 0 {
        0.0
    } else {
        cpu / received as f64 * 1e6
    };
    println!("protocol=quinn");
    println!("threads={}", args.threads);
    println!("payload_bytes={}", args.size);
    println!("submitted={submitted}");
    println!("received={received}");
    println!("delivery_percent={delivery_percent:.3}");
    println!("elapsed_seconds={elapsed:.3}");
    println!("ns_per_message={ns_per_message:.3}");
    println!("messages_per_second={messages_per_second:.3}");
    println!(
        "payload_MB_per_second={:.3}",
        messages_per_second * args.size as f64 / 1e6
    );
    println!("cpu_seconds={cpu:.3}");
    println!("cpu_us_per_message={cpu_us_per_message:.3}");
    Ok(())
}

async fn send_saturate_message(connection: &Connection, payload: &[u8]) -> Result<(), AnyError> {
    let mut stream = connection.open_uni().await?;
    stream.write_all(payload).await?;
    stream.finish()?;
    Ok(())
}

fn print_result(metrics: &Metrics) {
    println!(
        "RESULT role={} workload={} connections={} ready={} messages_sent={} messages_received={} queries_sent={} queries_completed={} errors={} cpu_seconds={:.6}",
        metrics.role.as_str(),
        metrics.workload.as_str(),
        metrics.counters.connections_current.load(Ordering::Relaxed),
        metrics.counters.connections_ready.load(Ordering::Relaxed),
        metrics.counters.messages_sent.load(Ordering::Relaxed),
        metrics.counters.messages_received.load(Ordering::Relaxed),
        metrics.counters.queries_sent.load(Ordering::Relaxed),
        metrics.counters.queries_completed.load(Ordering::Relaxed),
        metrics.counters.errors.load(Ordering::Relaxed),
        cpu_seconds(),
    );
    let _ = io::stdout().flush();
}

fn cpu_seconds() -> f64 {
    unsafe {
        let mut usage: libc::rusage = std::mem::zeroed();
        if libc::getrusage(libc::RUSAGE_SELF, &mut usage) != 0 {
            return 0.0;
        }
        let seconds = |time: libc::timeval| time.tv_sec as f64 + time.tv_usec as f64 / 1e6;
        seconds(usage.ru_utime) + seconds(usage.ru_stime)
    }
}

fn max_rss_bytes() -> u64 {
    unsafe {
        let mut usage: libc::rusage = std::mem::zeroed();
        if libc::getrusage(libc::RUSAGE_SELF, &mut usage) != 0 {
            return 0;
        }
        #[cfg(target_os = "macos")]
        return usage.ru_maxrss as u64;
        #[cfg(not(target_os = "macos"))]
        return (usage.ru_maxrss as u64).saturating_mul(1024);
    }
}

fn main() {
    if let Err(error) = real_main() {
        eprintln!("error: {error}");
        std::process::exit(1);
    }
}

fn real_main() -> Result<(), AnyError> {
    let args = parse_args().map_err(io::Error::other)?;
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(args.threads)
        .enable_all()
        .build()?;
    if args.workload == Workload::Saturate {
        return runtime.block_on(run_saturate(args));
    }
    match args.role {
        Role::Server => runtime.block_on(run_server(args)),
        Role::Client => runtime.block_on(run_client(args)),
    }
}
