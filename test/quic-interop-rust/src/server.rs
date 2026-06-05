mod rpk_config;

use log::{error, info};
use rustls::client::danger::HandshakeSignatureValid;
use rustls::server::danger::{ClientCertVerified, ClientCertVerifier};
use rustls::pki_types::{CertificateDer, UnixTime};
use rustls::{DigitallySignedStruct, DistinguishedName, Error, SignatureScheme};
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

const ALPN: &[u8] = b"hq-interop";

#[derive(Debug)]
struct OptionalRpkClientVerifier;

impl ClientCertVerifier for OptionalRpkClientVerifier {
    fn offer_client_auth(&self) -> bool {
        false
    }

    fn client_auth_mandatory(&self) -> bool {
        false
    }

    fn root_hint_subjects(&self) -> &[DistinguishedName] {
        &[]
    }

    fn verify_client_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _now: UnixTime,
    ) -> Result<ClientCertVerified, Error> {
        rpk_config::extract_ed25519_pubkey(end_entity.as_ref())
            .ok_or_else(|| Error::General("invalid Ed25519 raw public key".into()))?;
        Ok(ClientCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, Error> {
        Err(Error::General("TLS 1.2 not supported for RPK".into()))
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, Error> {
        if dss.scheme != SignatureScheme::ED25519 {
            return Err(Error::General(format!(
                "unsupported signature scheme: {:?}",
                dss.scheme
            )));
        }

        let pub_key = rpk_config::extract_ed25519_pubkey(cert.as_ref())
            .ok_or_else(|| Error::General("invalid Ed25519 cert in signature verify".into()))?;
        let key = ring::signature::UnparsedPublicKey::new(&ring::signature::ED25519, pub_key);
        key.verify(message, dss.signature())
            .map_err(|_| Error::General("Ed25519 signature verification failed".into()))?;
        Ok(HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        vec![SignatureScheme::ED25519]
    }

    fn requires_raw_public_keys(&self) -> bool {
        true
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::builder()
        .filter_level(log::LevelFilter::Info)
        .init();

    let bind_addr: SocketAddr = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "127.0.0.1:4433".to_string())
        .parse()?;

    let (server_key, server_pub) = rpk_config::generate_ed25519_key()?;
    info!("Server public key: {}", hex::encode(server_pub));
    let certified_key = rpk_config::make_certified_key(&server_key)?;
    let provider = rustls::crypto::ring::default_provider();
    let mut rustls_config = rustls::ServerConfig::builder_with_provider(provider.into())
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .with_client_cert_verifier(Arc::new(OptionalRpkClientVerifier))
        .with_cert_resolver(Arc::new(
            rustls::server::AlwaysResolvesServerRawPublicKeys::new(Arc::new(certified_key)),
        ));
    rustls_config.alpn_protocols = vec![ALPN.to_vec()];

    let mut server_config = quinn::ServerConfig::with_crypto(Arc::new(
        quinn::crypto::rustls::QuicServerConfig::try_from(rustls_config)?,
    ));
    let mut transport = quinn::TransportConfig::default();
    transport.max_idle_timeout(Some(Duration::from_secs(15).try_into()?));
    server_config.transport_config(Arc::new(transport));

    let endpoint = quinn::Endpoint::server(server_config, bind_addr)?;

    info!("Listening on {bind_addr} (ALPN: hq-interop)");

    while let Some(incoming) = endpoint.accept().await {
        tokio::spawn(async move {
            if let Err(err) = handle_connection(incoming).await {
                error!("connection failed: {err}");
            }
        });
    }

    Ok(())
}

async fn handle_connection(incoming: quinn::Incoming) -> Result<(), Box<dyn std::error::Error>> {
    let connection = incoming.await?;
    info!("Accepted connection from {}", connection.remote_address());

    loop {
        let stream = match connection.accept_bi().await {
            Ok(stream) => stream,
            Err(err) => {
                info!("Connection from {} closed: {err}", connection.remote_address());
                return Ok(());
            }
        };

        if let Err(err) = handle_request(stream).await {
            error!("request handling failed: {err}");
        }
    }
}

async fn handle_request(
    (mut send, mut recv): (quinn::SendStream, quinn::RecvStream),
) -> Result<(), Box<dyn std::error::Error>> {
    let request = recv.read_to_end(64 * 1024).await?;
    let first_line = String::from_utf8_lossy(&request)
        .lines()
        .next()
        .unwrap_or("<empty>")
        .to_string();
    info!("Received request: {first_line:?}");

    let body = format!("Hello from rpk-server (Rust)\nRequest: {first_line}\n");
    let response = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n\r\n{body}",
        body.len()
    );
    send.write_all(response.as_bytes()).await?;
    send.finish()?;
    Ok(())
}
