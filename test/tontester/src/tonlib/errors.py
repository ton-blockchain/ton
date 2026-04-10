from typing import override


class LocalError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code: int = code
        self.message: str = message

    @override
    def __str__(self):
        return f"LocalError(code={self.code}, message={self.message!r})"


class RemoteError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code: int = code
        self.message: str = message

    @override
    def __str__(self):
        return f"RemoteError(code={self.code}, message={self.message!r})"
