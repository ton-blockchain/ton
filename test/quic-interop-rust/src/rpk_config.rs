use ring::rand::SystemRandom;
use ring::signature::{Ed25519KeyPair, KeyPair};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::pki_types::{CertificateDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::sign::CertifiedKey;
use rustls::{DigitallySignedStruct, Error, SignatureScheme};

const ED25519_SPKI_PREFIX: [u8; 12] = [
    0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00,
];

pub fn generate_ed25519_key() -> Result<(Vec<u8>, [u8; 32]), Box<dyn std::error::Error>> {
    let rng = SystemRandom::new();
    let pkcs8 = Ed25519KeyPair::generate_pkcs8(&rng).map_err(|e| format!("keygen: {e}"))?;
    let key_pair =
        Ed25519KeyPair::from_pkcs8(pkcs8.as_ref()).map_err(|e| format!("parse key: {e}"))?;
    let pub_bytes: [u8; 32] = key_pair
        .public_key()
        .as_ref()
        .try_into()
        .map_err(|_| "Ed25519 public key not 32 bytes")?;
    Ok((pkcs8.as_ref().to_vec(), pub_bytes))
}

pub fn make_certified_key(pkcs8_der: &[u8]) -> Result<CertifiedKey, Box<dyn std::error::Error>> {
    let private_key = PrivatePkcs8KeyDer::from(pkcs8_der.to_vec());
    let provider = rustls::crypto::ring::default_provider();
    let signing_key = provider
        .key_provider
        .load_private_key(private_key.into())
        .map_err(|e| format!("load private key: {e}"))?;
    let spki_der = signing_key
        .public_key()
        .ok_or("no public key from signing key")?;
    let cert = CertificateDer::from(spki_der.as_ref().to_vec());
    Ok(CertifiedKey::new(vec![cert], signing_key))
}

pub(crate) fn extract_ed25519_pubkey(data: &[u8]) -> Option<&[u8]> {
    if data.len() == 44 && data.starts_with(&ED25519_SPKI_PREFIX) {
        Some(&data[12..])
    } else if data.len() == 32 {
        Some(data)
    } else {
        None
    }
}

#[allow(dead_code)]
#[derive(Debug)]
pub struct RpkServerVerifier {
    expected_pub: [u8; 32],
}

#[allow(dead_code)]
impl RpkServerVerifier {
    pub fn new(server_pub: [u8; 32]) -> Self {
        Self {
            expected_pub: server_pub,
        }
    }
}

impl ServerCertVerifier for RpkServerVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, Error> {
        let data = end_entity.as_ref();
        let pub_key = extract_ed25519_pubkey(data).ok_or_else(|| {
            Error::General(format!(
                "unrecognized server cert format ({} bytes)",
                data.len(),
            ))
        })?;
        if pub_key == self.expected_pub.as_slice() {
            Ok(ServerCertVerified::assertion())
        } else {
            Err(Error::General(format!(
                "server public key mismatch: expected {}, got {}",
                hex::encode(self.expected_pub),
                hex::encode(pub_key)
            )))
        }
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
        let data = cert.as_ref();
        let pub_key = extract_ed25519_pubkey(data)
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
