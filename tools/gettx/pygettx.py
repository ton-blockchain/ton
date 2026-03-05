"""
Python bindings for gettx - TON transaction lookup tool

This module provides a Python interface to lookup transactions and blocks
from TON validator databases using the gettx executable.
"""

import json
import os
import pathlib
import subprocess
from typing import Optional, Dict, Any, List


class GetTxError(Exception):
    """Exception raised when gettx operation fails"""
    pass


class GetTxClient:
    """Wrapper for the gettx executable"""

    def __init__(self, executable_path: Optional[str] = None):
        """
        Locate the gettx executable

        Args:
            executable_path: Path to the gettx executable. If None, searches in standard locations.
        """
        if executable_path is None:
            executable_path = self._find_executable()

        self._executable = executable_path
        self._db_path: Optional[str] = None
        self._include_deleted = False

    def _find_executable(self) -> str:
        """Find the gettx executable in standard locations"""
        # Check build directory (from script location: tools/gettx/)
        script_dir = pathlib.Path(__file__).parent
        build_exe = script_dir.parent.parent / "build" / "tools" / "gettx" / "gettx"
        if build_exe.exists():
            return str(build_exe)

        # Check if script is in build directory
        if "build" in script_dir.parts:
            # Try relative path from build dir
            build_exe = script_dir / "gettx"
            if build_exe.exists():
                return str(build_exe)

        # Check current directory
        if os.path.exists("gettx"):
            return "gettx"

        # Check system PATH
        for path_dir in os.environ.get("PATH", "").split(os.pathsep):
            exe_path = os.path.join(path_dir, "gettx")
            if os.path.exists(exe_path) and os.access(exe_path, os.X_OK):
                return exe_path

        raise FileNotFoundError(
            "Could not find gettx executable. Please build it first or specify executable_path.\n"
            "Build with: cd build && cmake --build . --target gettx"
        )

    def open(self, db_path: str, include_deleted: bool = False) -> None:
        """
        Set the database path for subsequent operations

        Args:
            db_path: Path to the validator database root directory
            include_deleted: If True, include packages marked as deleted in search
        """
        self._db_path = db_path
        self._include_deleted = include_deleted

    def close(self) -> None:
        """Close the database and free resources"""
        self._db_path = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _run_command(self, args: List[str]) -> Dict[str, Any]:
        """
        Run gettx command and parse JSON output

        Args:
            args: Command arguments (without subcommand)

        Returns:
            Parsed JSON output

        Raises:
            GetTxError: If the command fails
        """
        if self._db_path is None:
            raise GetTxError("Database not opened. Call open() first.")

        cmd = [self._executable]
        cmd.extend(args)
        cmd.extend(["--db-path", self._db_path])

        if self._include_deleted:
            cmd.append("--include-deleted")

        try:
            result = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=True
            )

            # Parse JSON from stdout (stderr contains debug messages)
            return json.loads(result.stdout)

        except subprocess.CalledProcessError as e:
            # Include both stdout and stderr in error message
            error_msg = f"Exit code {e.returncode}"
            if e.stderr:
                error_msg += f" - stderr: {e.stderr.strip()}"
            if e.stdout:
                error_msg += f" - stdout: {e.stdout.strip()}"
            raise GetTxError(f"gettx command failed: {error_msg}")

        except json.JSONDecodeError as e:
            # Provide more context about parsing failure
            stdout_preview = result.stdout[:200] if result.stdout else "(empty)"
            raise GetTxError(f"Failed to parse JSON output: {e}\nOutput preview: {stdout_preview}")

    def get_transactions(
        self,
        workchain: int,
        address: str,
        logical_time: int,
        tx_hash: str,
        count: int = 10
    ) -> Dict[str, Any]:
        """
        Lookup transactions from the database (tx subcommand)

        Args:
            workchain: Workchain ID (e.g., -1 for masterchain)
            address: Account address in hex format
            logical_time: Logical time of the transaction
            tx_hash: Transaction hash in hex format
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
        args = [
            "tx",
            "--workchain", str(workchain),
            "--address", address,
            "--lt", str(logical_time),
            "--hash", tx_hash,
            "--count", str(count)
        ]

        return self._run_command(args)

    def get_block_transactions(
        self,
        mc_seqno: int
    ) -> Dict[str, Any]:
        """
        Get all transactions from a masterchain block by seqno (block subcommand)

        Args:
            mc_seqno: Masterchain block sequence number

        Returns:
            Dictionary with block transaction data:
            {
                "mc_seqno": ...,
                "mc_block_id": "...",
                "shard_count": ...,
                "total_transactions": ...,
                "transactions": [
                    {
                        "transaction_id": {"account": "...", "lt": ..., "hash": "..."},
                        "workchain": ...,
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
        args = [
            "block",
            "--seqno", str(mc_seqno)
        ]

        return self._run_command(args)


# Global client instance
_client_instance: Optional[GetTxClient] = None


def get_transactions(
    db_path: str,
    workchain: int,
    address: str,
    logical_time: int,
    tx_hash: str,
    count: int = 10,
    include_deleted: bool = False
) -> Dict[str, Any]:
    """
    Convenience function to lookup transactions by address/LT without managing the client explicitly

    Args:
        db_path: Path to the validator database root directory
        workchain: Workchain ID (e.g., -1 for masterchain)
        address: Account address in hex format
        logical_time: Logical time of the transaction
        tx_hash: Transaction hash in hex format
        count: Number of transactions to retrieve (default: 10)
        include_deleted: If True, include packages marked as deleted in search (default: False)

    Returns:
        Dictionary with transaction data

    Example:
        >>> result = get_transactions(
        ...     db_path="/var/ton/db",
        ...     workchain=-1,
        ...     address="34517C7BDF5187C55AF4F8B61FDC321588C7AB768DEE24B006DF29106458D7CF",
        ...     logical_time=2000001,
        ...     tx_hash="EBD5D309774BAC43AD60907BEA1AC08B9AF1D9F27CD24165F0CC1E7EB2E33585",
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
) -> Dict[str, Any]:
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
        print(f"Commands:")
        print(f"  tx <db_path> <workchain> <address> <lt> <hash> [count]")
        print(f"      Lookup transactions by address and logical time")
        print(f"  block <db_path> <mc_seqno>")
        print(f"      Get all transactions from a masterchain block by seqno")
        print(f"\nExamples:")
        print(f"  {sys.argv[0]} tx /var/ton/db -1 34517C7BDF5187C55AF4F8B61FDC321588C7AB768DEE24B006DF29106458D7CF 2000001 EBD5D309774BAC43AD60907BEA1AC08B9AF1D9F27CD24165F0CC1E7EB2E33585 1")
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
            print(f"Valid commands: tx, block")
            sys.exit(1)

    except GetTxError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
