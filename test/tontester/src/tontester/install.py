import os
import subprocess
from pathlib import Path
from typing import final


@final
class Install:
    def __init__(self, build_dir: Path, source_dir: Path):
        self._build_dir = build_dir.absolute()
        self._source_dir = source_dir.absolute()

    @property
    def build_dir(self):
        return self._build_dir

    @property
    def source_dir(self):
        return self._source_dir

    @property
    def fift_exe(self):
        return self.build_dir / "crypto/create-state"

    @property
    def fift_include_dirs(self):
        return [
            self.source_dir / "crypto/fift/lib",
            self.source_dir / "crypto/smartcont",
        ]

    @property
    def key_helper_exe(self):
        return self.build_dir / "utils/generate-random-id"

    @property
    def validator_engine_exe(self):
        return self.build_dir / "validator-engine/validator-engine"

    @property
    def dht_server_exe(self):
        return self.build_dir / "dht-server/dht-server"

    @property
    def tonlibjson(self):
        return self.build_dir / "tonlib/libtonlibjson.so"


def run_fift(install: Install, code: str, working_dir: Path):
    script_file = working_dir / "script.fif"
    _ = script_file.write_text(code)

    args = [install.fift_exe]
    for include_dir in install.fift_include_dirs:
        args += ["-I", include_dir]
    args += ["-s", "script.fif"]

    _ = subprocess.run(args, cwd=working_dir, check=True)

    os.remove(script_file)
