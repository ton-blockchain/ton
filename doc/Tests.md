# Tests execution
TON contains multiple unit-tests, that facilitate detection of erroneous blockchain behaviour on each commit.
## Build tests
Go inside the build directory and, if you use ninja, build the tests using the following command:

```ninja test-ed25519 test-ed25519-crypto test-bigint test-vm test-fift test-cells test-smartcont test-net test-tdactor test-tdutils test-tonlib-offline test-adnl test-dht test-rldp test-rldp2 test-catchain test-fec test-tddb test-db test-validator-session-state```

For more details on how to build TON artifacts, please refer to any of Github actions.

For cmake use:

```cmake --build . --target test-ed25519 test-ed25519-crypto test-bigint test-vm test-fift test-cells test-smartcont test-net test-tdactor test-tdutils test-tonlib-offline test-adnl test-dht test-rldp test-rldp2 test-catchain test-fec test-tddb test-db test-validator-session-state```

## Run tests
Go inside the build directory and with ninja execute:

```ninja test```

with ctest:

```ctest```

## Integration of tests into CI
Most relevant GitHub actions include the step ```Run tests``` that executes the tests. If any of tests fails, the action will be interrupted and no artifacts will be provided.