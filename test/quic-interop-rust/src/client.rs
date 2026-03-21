mod rpk_config;

use log::{info, warn};
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::builder()
        .filter_level(log::LevelFilter::Info)
        .init();

    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("Usage: {} <server_addr> <server_pubkey>", args[0]);
        eprintln!("  server_pubkey: hex (64 chars) or base64 (44 chars) Ed25519 public key");
        std::process::exit(1);
    }
    let server_addr: SocketAddr = args[1].parse()?;
    let server_pub_str = &args[2];
    let server_pub_bytes = if server_pub_str.len() == 64
        && server_pub_str.chars().all(|c| c.is_ascii_hexdigit())
    {
        hex::decode(server_pub_str)?
    } else {
        use base64::Engine;
        base64::engine::general_purpose::STANDARD.decode(server_pub_str)?
    };
    let server_pub: [u8; 32] = server_pub_bytes
        .try_into()
        .map_err(|_| "server pubkey must be 32 bytes")?;

    info!("Connecting to {server_addr}");
    info!("Server pubkey: {}", hex::encode(server_pub));

    let (client_key, client_pub) = rpk_config::generate_ed25519_key()?;
    info!("Client public key: {}", hex::encode(client_pub));

    // Build rustls ClientConfig with RPK
    let certified_key = rpk_config::make_certified_key(&client_key)?;
    let verifier = Arc::new(rpk_config::RpkServerVerifier::new(server_pub));

    let provider = rustls::crypto::ring::default_provider();
    let mut rustls_config = rustls::ClientConfig::builder_with_provider(provider.into())
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_client_cert_resolver(Arc::new(
            rustls::client::AlwaysResolvesClientRawPublicKeys::new(Arc::new(certified_key)),
        ));
    rustls_config.alpn_protocols = vec![b"hq-interop".to_vec()];

    // Build Quinn config
    let quic_client_config =
        quinn::crypto::rustls::QuicClientConfig::try_from(Arc::new(rustls_config))?;
    let mut client_config = quinn::ClientConfig::new(Arc::new(quic_client_config));

    let mut transport = quinn::TransportConfig::default();
    transport.max_idle_timeout(Some(Duration::from_secs(15).try_into()?));
    client_config.transport_config(Arc::new(transport));

    // Endpoint config with short CID lifetime to force CID rotation
    let mut endpoint_config = quinn::EndpointConfig::default();
    endpoint_config.cid_generator(|| {
        let mut gen = quinn_proto::HashedConnectionIdGenerator::new();
        gen.set_lifetime(Duration::from_secs(2));
        Box::new(gen)
    });

    // Create endpoint
    let socket = std::net::UdpSocket::bind("0.0.0.0:0")?;
    let mut endpoint = quinn::Endpoint::new(
        endpoint_config,
        None,
        socket,
        Arc::new(quinn::TokioRuntime),
    )?;
    endpoint.set_default_client_config(client_config);

    info!("Connecting...");
    let connection: quinn::Connection = endpoint.connect(server_addr, "localhost")?.await?;
    info!("=== Handshake complete ===");
    info!("Remote address: {}", connection.remote_address());

    // Phase 2: First request
    info!("--- Sending first request ---");
    let resp1 = do_request(&connection, b"GET /first HTTP/1.1\r\nHost: test\r\n\r\n").await?;
    info!("Response 1:\n{}", String::from_utf8_lossy(&resp1));

    // Phase 3: Path migration (forces DCID rotation per QUIC spec)
    info!("--- Path migration (forces DCID rotation) ---");
    let new_socket = std::net::UdpSocket::bind("0.0.0.0:0")?;
    let new_addr = new_socket.local_addr()?;
    info!("Rebinding to {new_addr}");
    endpoint.rebind(new_socket)?;

    info!("--- Sending second request (post-migration, new DCID) ---");
    match tokio::time::timeout(Duration::from_secs(5), do_request(&connection, b"GET /second HTTP/1.1\r\nHost: test\r\n\r\n")).await {
        Ok(Ok(resp2)) => {
            info!("Response 2:\n{}", String::from_utf8_lossy(&resp2));
            info!("  Phase 3: Path migration + DCID rotation [OK]");
        }
        _ => {
            warn!("Path migration response timed out — check server logs for 'alt CID' routing!");
        }
    }

    // Phase 4: Wait for CID lifetime expiry (2s) to trigger local CID rotation
    info!("--- Waiting 3s for CID lifetime expiry (local CID rotation) ---");
    tokio::time::sleep(Duration::from_secs(3)).await;

    info!("--- Sending third request (after CID rotation) ---");
    match tokio::time::timeout(Duration::from_secs(5), do_request(&connection, b"GET /third HTTP/1.1\r\nHost: test\r\n\r\n")).await {
        Ok(Ok(resp3)) => {
            info!("Response 3:\n{}", String::from_utf8_lossy(&resp3));
            info!("  Phase 4: CID rotation via lifetime [OK]");
        }
        Ok(Err(e)) => warn!("Request 3 error: {e}"),
        Err(_) => warn!("Request 3 timed out"),
    }

    info!("=== DONE ===");
    connection.close(0u32.into(), b"done");
    let _ = tokio::time::timeout(Duration::from_secs(1), endpoint.wait_idle()).await;
    Ok(())
}

async fn do_request(
    connection: &quinn::Connection,
    request: &[u8],
) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    let (mut send, mut recv) = connection.open_bi().await?;
    send.write_all(request).await?;
    send.finish()?;
    let resp = recv.read_to_end(65535).await?;
    Ok(resp)
}
