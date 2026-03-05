# gettx - Standalone TON Transaction Lookup Tool

A standalone C++/Python tool that provides `liteserver.getTransactions()` functionality by directly reading TON validator database files.

## Building

```bash
cd /path/to/ton
mkdir build && cd build
cmake ..
cmake --build . --target gettx
```

## Usage

```bash
./gettx \
  --workchain <w> \
  --address <addr> \
  --lt <logical_time> \
  --hash <tx_hash> \
  --count <n> \
  --db-path <path> \
  --format <json|tl>
```
