#!/usr/bin/env python3
"""
Tests for pygettx Python bindings using FFI wrapper
"""

import json
import sys
import os

# Add parent directory to path to import pygettx
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pygettx


def test_tx_subcommand():
    """Test transaction lookup using FFI wrapper"""
    print("Testing tx subcommand with FFI...")

    try:
        result = pygettx.get_transactions(
            db_path="../../db",
            workchain=-1,
            address="Ef80UXx731GHxVr0-LYf3DIViMerdo3uJLAG3ykQZFjXz2kW",
            logical_time=2000001,
            tx_hash="69XTCXdLrEOtYJB76hrAi5rx2fJ80kFl8MwefrLjNYU=",
            count=1
        )

        # Validate structure
        assert "transactions" in result, "Missing 'transactions' key"
        assert len(result["transactions"]) == 1, f"Expected exactly 1 transaction, got {len(result['transactions'])}"

        tx = result["transactions"][0]
        assert "transaction_id" in tx, "Missing 'transaction_id' key"
        assert "account" in tx["transaction_id"], "Missing 'account' in transaction_id"
        assert "lt" in tx["transaction_id"], "Missing 'lt' in transaction_id"
        assert "hash" in tx["transaction_id"], "Missing 'hash' in transaction_id"
        assert tx["transaction_id"]["lt"] == 2000001, f"Wrong LT: {tx['transaction_id']['lt']}"
        assert "fee" in tx, "Missing 'fee' key"
        assert "utime" in tx, "Missing 'utime' key"
        assert "block" in tx, "Missing 'block' key"

        print(f"✓ Transaction lookup successful!")
        print(f"  Account: {tx['transaction_id']['account']}")
        print(f"  LT: {tx['transaction_id']['lt']}")
        print(f"  Hash: {tx['transaction_id']['hash']}")
        print(f"  Fee: {tx['fee']}")
        print(f"  Block: {tx['block']}")

        return True

    except pygettx.GetTxError as e:
        print(f"✗ Transaction lookup failed: {e}")
        return False
    except AssertionError as e:
        print(f"✗ Assertion failed: {e}")
        return False
    except Exception as e:
        print(f"✗ Unexpected error: {e}")
        import traceback
        traceback.print_exc()
        return False


def test_block_subcommand():
    """Test block transaction lookup using FFI wrapper"""
    print("\nTesting block subcommand with FFI...")

    try:
        result = pygettx.get_block_transactions(
            db_path="../../db",
            mc_seqno=2
        )

        # Validate structure
        assert "mc_seqno" in result, "Missing 'mc_seqno' key"
        assert result["mc_seqno"] == 2, f"Expected mc_seqno=2, got {result['mc_seqno']}"
        assert "mc_block_id" in result, "Missing 'mc_block_id' key"
        assert "shard_count" in result, "Missing 'shard_count' key"
        assert "total_transactions" in result, "Missing 'total_transactions' key"
        assert "transactions" in result, "Missing 'transactions' key"
        assert len(result["transactions"]) == 11, f"Expected exactly 11 transactions, got {len(result['transactions'])}"
        assert result["total_transactions"] == 11, f"Expected total_transactions=11, got {result['total_transactions']}"

        print(f"✓ Block lookup successful!")
        print(f"  MC Seqno: {result['mc_seqno']}")
        print(f"  MC Block ID: {result['mc_block_id']}")
        print(f"  Shard Count: {result['shard_count']}")
        print(f"  Total Transactions: {result['total_transactions']}")

        # Show first few transactions
        for i, tx in enumerate(result['transactions'][:3]):
            print(f"  [{i}] {tx['transaction_id']['account']}: lt={tx['transaction_id']['lt']}")

        if len(result['transactions']) > 3:
            print(f"  ... and {len(result['transactions']) - 3} more")

        return True

    except pygettx.GetTxError as e:
        print(f"✗ Block lookup failed: {e}")
        return False
    except AssertionError as e:
        print(f"✗ Assertion failed: {e}")
        return False
    except Exception as e:
        print(f"✗ Unexpected error: {e}")
        import traceback
        traceback.print_exc()
        return False


def test_client_context_manager():
    """Test context manager usage"""
    print("\nTesting client context manager...")

    try:
        with pygettx.GetTxClient() as client:
            client.open("../../db")
            result = client.get_transactions(
                workchain=-1,
                address="Ef80UXx731GHxVr0-LYf3DIViMerdo3uJLAG3ykQZFjXz2kW",
                logical_time=2000001,
                tx_hash="69XTCXdLrEOtYJB76hrAi5rx2fJ80kFl8MwefrLjNYU=",
                count=1
            )
            assert "transactions" in result

        print("✓ Context manager test passed!")
        return True

    except Exception as e:
        print(f"✗ Context manager test failed: {e}")
        import traceback
        traceback.print_exc()
        return False


def main():
    """Run all tests"""
    print("=" * 60)
    print("pygettx FFI Wrapper Tests")
    print("=" * 60)

    # Change to script directory
    os.chdir(os.path.dirname(os.path.abspath(__file__)))

    results = []

    # Run tests
    results.append(("tx subcommand", test_tx_subcommand()))
    results.append(("block subcommand", test_block_subcommand()))
    results.append(("context manager", test_client_context_manager()))

    # Print summary
    print("\n" + "=" * 60)
    print("Test Summary")
    print("=" * 60)

    for name, passed in results:
        status = "✓ PASS" if passed else "✗ FAIL"
        print(f"{status}: {name}")

    total = len(results)
    passed = sum(1 for _, p in results if p)

    print(f"\nTotal: {passed}/{total} tests passed")

    if passed == total:
        print("\n🎉 All tests passed!")
        return 0
    else:
        print(f"\n❌ {total - passed} test(s) failed")
        return 1


if __name__ == "__main__":
    sys.exit(main())
