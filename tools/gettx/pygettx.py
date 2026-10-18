"""
Python bindings for gettx - TON transaction lookup tool

This module provides a Python interface to lookup transactions and blocks
from TON validator databases using the FFI wrapper (libgettx.so).
"""

import ctypes
import json
import pathlib
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    import types

JsonDict = dict[str, Any]  # type: ignore


class GetTxError(Exception):
    """Exception raised when gettx operation fails"""
    pass


class GetTxClient:
    """Wrapper using FFI to call libgettx.so shared library"""

    _lib: ctypes.CDLL
    _handle: ctypes.c_void_p | None
    _db_path: str | None
    _include_deleted: bool
    _GetTxResult: type[ctypes.Structure]

    def __init__(self, lib_path: str | None = None):
        """
        Initialize the FFI client

        Args:
            lib_path: Path to libgettx.so shared library. If None, searches in standard locations.
        """
        if lib_path is None:
            lib_path = self._find_library()

        self._lib = ctypes.CDLL(lib_path)
        self._handle = None
        self._db_path = None
        self._include_deleted = False

        # Define result structure
        class GetTxResult(ctypes.Structure):  # type: ignore
            _fields_ = [
                ("json_data", ctypes.c_char_p),
                ("error_code", ctypes.c_int),
                ("error_msg", ctypes.c_char_p),
            ]

        self._GetTxResult = GetTxResult

        # Configure function signatures
        self._lib.gettx_create.argtypes = [ctypes.c_char_p, ctypes.c_int]
        self._lib.gettx_create.restype = ctypes.c_void_p

        self._lib.gettx_lookup.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_ulonglong,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        self._lib.gettx_lookup.restype = self._GetTxResult

        self._lib.gettx_lookup_block.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
        ]
        self._lib.gettx_lookup_block.restype = self._GetTxResult

        self._lib.gettx_free_result.argtypes = [ctypes.POINTER(self._GetTxResult)]
        self._lib.gettx_free_result.restype = None

        self._lib.gettx_destroy.argtypes = [ctypes.c_void_p]
        self._lib.gettx_destroy.restype = None

    def _find_library(self) -> str:
        """Find the libgettx.so shared library"""
        script_dir = pathlib.Path(__file__).parent
        build_lib = script_dir.parent.parent / "build" / "tools" / "gettx" / "libgettx.so"
        if build_lib.exists():
            return str(build_lib)

        # Check common library paths
        common_paths = [
            "/usr/lib/x86_64-linux-gnu/libgettx.so",
            "/usr/local/lib/libgettx.so",
        ]

        for path in common_paths:
            if pathlib.Path(path).exists():
                return path

        # Try LD_LIBRARY_PATH
        import os
        lib_path = os.environ.get('LD_LIBRARY_PATH')
        if lib_path:
            for name in ['libgettx.so', 'libgettx.so.1']:
                full_path = os.path.join(lib_path, name)
                if os.path.exists(full_path):
                    return full_path

        raise FileNotFoundError(
            "Cannot find libgettx.so shared library. "
            + "Please build it first or specify lib_path.\n"
            + "Build with: cd build && cmake --build . --target gettx\n"
            + "Then look for: build/tools/gettx/libgettx.so"
        )

    def open(self, db_path: str, include_deleted: bool = False) -> None:
        """
        Set the database path for subsequent operations

        Args:
            db_path: Path to the validator database root directory
            include_deleted: If True, include packages marked as deleted in search
        """
        if self._handle is not None:
            self.close()

        self._db_path = db_path
        self._include_deleted = include_deleted

        self._handle = self._lib.gettx_create(db_path.encode('utf-8'), int(include_deleted))
        if not self._handle:
            raise GetTxError(f"Failed to create gettx context for database: {db_path}")

    def close(self) -> None:
        """Close the database and free resources"""
        if self._handle is not None:
            self._lib.gettx_destroy(self._handle)
            self._handle = None
        self._db_path = None

    def __enter__(self):
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: types.TracebackType | None,
    ) -> None:
        self.close()

    def get_transactions(
        self,
        workchain: int,
        address: str,
        logical_time: int,
        tx_hash: str,
        count: int = 10
    ) -> JsonDict:
        """
        Lookup transactions from the database using FFI

        Args:
            workchain: Workchain ID (e.g., -1 for masterchain)
            address: Account address in base64url format
            logical_time: Logical time of the transaction
            tx_hash: Transaction hash in base64 or base64url format
            count: Number of transactions to retrieve (default: 10)

        Returns:
            Dictionary with transaction data matching raw.getTransactions format:
            {
                "transactions": [
                    {
                        "transaction_id": {"account": "...", "lt": ..., "hash": "..."},
                        "fee": ...,
                        "utime": ...,
                        "in_msg": "..." or None,
                        "out_msgs": ["...", ...],
                        "data": "...",
                        "block": "..."
                    },
                    ...
                ]
            }

        Raises:
            GetTxError: If the lookup fails
        """
        if self._handle is None:
            raise GetTxError("Database not opened. Call open() first.")

        result: Any = self._lib.gettx_lookup(  # type: ignore
            self._handle,
            workchain,
            address.encode('utf-8'),
            logical_time,
            tx_hash.encode('utf-8'),
            count
        )

        try:
            if result.error_code != 0:  # type: ignore
                error_msg = result.error_msg.decode('utf-8') if result.error_msg else "Unknown error"
                raise GetTxError(f"gettx_lookup failed (code {result.error_code}): {error_msg}")

            if not result.json_data:  # type: ignore
                raise GetTxError("gettx_lookup returned empty data")

            data = json.loads(result.json_data.decode('utf-8'))  # type: ignore
            return data  # type: ignore

        finally:
            # Always free the result
            self._lib.gettx_free_result(ctypes.byref(result))  # type: ignore

    def get_block_transactions(
        self,
        mc_seqno: int
    ) -> JsonDict:
        """
        Get all transactions from a masterchain block by seqno using FFI

        Args:
            mc_seqno: Masterchain block sequence number

        Returns:
            Dictionary with block transaction data:
            {
                "mc_seqno": ...,
                "mc_block_id": "...",
                "shard_count": ...,
                "total_transactions": ...,
                "transactions": [...]
            }

        Raises:
            GetTxError: If the lookup fails
        """
        if self._handle is None:
            raise GetTxError("Database not opened. Call open() first.")

        result: Any = self._lib.gettx_lookup_block(  # type: ignore
            self._handle,
            mc_seqno
        )

        try:
            if result.error_code != 0:  # type: ignore
                error_msg = result.error_msg.decode('utf-8') if result.error_msg else "Unknown error"
                raise GetTxError(f"gettx_lookup_block failed (code {result.error_code}): {error_msg}")

            if not result.json_data:  # type: ignore
                raise GetTxError("gettx_lookup_block returned empty data")

            data = json.loads(result.json_data.decode('utf-8'))  # type: ignore
            return data  # type: ignore

        finally:
            # Always free the result
            self._lib.gettx_free_result(ctypes.byref(result))  # type: ignore


# Global client instance
_client_instance: GetTxClient | None = None


def get_transactions(
    db_path: str,
    workchain: int,
    address: str,
    logical_time: int,
    tx_hash: str,
    count: int = 10,
    include_deleted: bool = False
) -> JsonDict:
    """
    Convenience function to lookup transactions by address/LT without managing the client explicitly

    Args:
        db_path: Path to the validator database root directory
        workchain: Workchain ID (e.g., -1 for masterchain)
        address: Account address in base64url format
        logical_time: Logical time of the transaction
        tx_hash: Transaction hash in base64 or base64url format
        count: Number of transactions to retrieve (default: 10)
        include_deleted: If True, include packages marked as deleted in search (default: False)

    Returns:
        Dictionary with transaction data

    Example:
        >>> result = get_transactions(
        ...     db_path="/var/ton/db",
        ...     workchain=-1,
        ...     address="Ef80UXx731GHxVr0-LYf3DIViMerdo3uJLAG3ykQZFjXz2kW",
        ...     logical_time=2000001,
        ...     tx_hash="69XTCXdLrEOtYJB76hrAi5rx2fJ80kFl8MwefrLjNYU=",
        ...     count=1
        ... )
        >>> print(result['transactions'][0]['transaction_id']['hash'])
    """
    global _client_instance

    if _client_instance is None:
        _client_instance = GetTxClient()

    with _client_instance:
        _client_instance.open(db_path, include_deleted=include_deleted)
        return _client_instance.get_transactions(workchain, address, logical_time, tx_hash, count)


def get_block_transactions(
    db_path: str,
    mc_seqno: int,
    include_deleted: bool = False
) -> JsonDict:
    """
    Convenience function to get all transactions from a masterchain block by seqno

    Args:
        db_path: Path to the validator database root directory
        mc_seqno: Masterchain block sequence number
        include_deleted: If True, include packages marked as deleted in search (default: False)

    Returns:
        Dictionary with block transaction data including metadata

    Example:
        >>> result = get_block_transactions(
        ...     db_path="/var/ton/db",
        ...     mc_seqno=2
        ... )
        >>> print(f"Found {result['total_transactions']} transactions")
        >>> for tx in result['transactions']:
        ...     print(f"  {tx['transaction_id']['account']}: lt={tx['transaction_id']['lt']}")
    """
    global _client_instance

    if _client_instance is None:
        _client_instance = GetTxClient()

    with _client_instance:
        _client_instance.open(db_path, include_deleted=include_deleted)
        return _client_instance.get_block_transactions(mc_seqno)


if __name__ == "__main__":
    # Command-line interface
    import sys

    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <command> [args...]")
        print("Commands:")
        print("  tx <db_path> <workchain> <address> <lt> <hash> [count]")
        print("      Lookup transactions by address and logical time")
        print("  block <db_path> <mc_seqno>")
        print("      Get all transactions from a masterchain block by seqno")
        print("\nExamples:")
        print(f"  {sys.argv[0]} tx /var/ton/db -1 Ef80UXx731GHxVr0-LYf3DIViMerdo3uJLAG3ykQZFjXz2kW 2000001 69XTCXdLrEOtYJB76hrAi5rx2fJ80kFl8MwefrLjNYU= 1")
        print(f"  {sys.argv[0]} block /var/ton/db 2")
        sys.exit(1)

    command = sys.argv[1]

    try:
        if command == "tx":
            # tx subcommand
            if len(sys.argv) < 7:
                print(f"Usage: {sys.argv[0]} tx <db_path> <workchain> <address> <lt> <hash> [count]")
                sys.exit(1)

            db_path = sys.argv[2]
            workchain = int(sys.argv[3])
            address = sys.argv[4]
            lt = int(sys.argv[5])
            tx_hash = sys.argv[6]
            count = int(sys.argv[7]) if len(sys.argv) > 7 else 1

            result = get_transactions(db_path, workchain, address, lt, tx_hash, count)
            print(json.dumps(result, indent=2))

        elif command == "block":
            # block subcommand
            if len(sys.argv) < 4:
                print(f"Usage: {sys.argv[0]} block <db_path> <mc_seqno>")
                sys.exit(1)

            db_path = sys.argv[2]
            mc_seqno = int(sys.argv[3])

            result = get_block_transactions(db_path, mc_seqno)
            print(json.dumps(result, indent=2))

        else:
            print(f"Error: Unknown command '{command}'")
            print("Valid commands: tx, block")
            sys.exit(1)

    except GetTxError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
