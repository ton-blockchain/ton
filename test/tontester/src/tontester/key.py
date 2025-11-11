import base64
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .install import Install
from .tl import tonapi


@dataclass
class Key:
    private_key: tonapi.pk_ed25519
    public_key: tonapi.pub_ed25519
    short_key: tonapi.adnl_id_short

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
            private_key=tonapi.pk_ed25519.model_validate_json(parts[0].decode()),
            public_key=tonapi.pub_ed25519.model_validate_json(parts[1].decode()),
            short_key=tonapi.adnl_id_short.model_validate_json(parts[2].decode()),
        )

    def add_to_keyring(self, keyring: Path) -> Path:
        file = keyring / self.short_key.id.hex().upper()
        _ = file.write_bytes(self.private_key.tl_tag + self.private_key.key)
        return file

    def id(self):
        return base64.b64encode(self.short_key.id)
