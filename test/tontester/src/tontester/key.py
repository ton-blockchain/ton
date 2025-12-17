import subprocess
from dataclasses import dataclass
from pathlib import Path

from .install import Install
from .tl import ton_api


@dataclass
class Key:
    private_key: ton_api.Pk_ed25519
    public_key: ton_api.Pub_ed25519
    short_key: ton_api.Adnl_id_short

    @staticmethod
    def new(install: Install) -> "Key":
        result = subprocess.run(
            (install.key_helper_exe, "-m", "id"),
            check=True,
            stdout=subprocess.PIPE,
        ).stdout

        parts = result.splitlines()
        if len(parts) != 3:
            raise ValueError("Invalid key generation output")

        return Key(
            private_key=ton_api.Pk_ed25519.from_json(parts[0].decode()),
            public_key=ton_api.Pub_ed25519.from_json(parts[1].decode()),
            short_key=ton_api.Adnl_id_short.from_json(parts[2].decode()),
        )

    def add_to_keyring(self, keyring: Path) -> Path:
        file = keyring / self.short_key.id.hex().upper()
        _ = file.write_bytes(b"\x17\x23\x68\x49" + self.private_key.key)
        return file

    def id(self):
        return self.short_key.id
