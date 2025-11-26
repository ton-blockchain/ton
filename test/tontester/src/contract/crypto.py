from nacl.signing import SigningKey


def private_key_to_public_key(priv_k: bytes) -> bytes:
    return SigningKey(priv_k).verify_key.encode()


def sign_message(message: bytes, signing_key: bytes) -> bytes:
    return SigningKey(signing_key).sign(message).signature
