# Transaction Emulator

Transaction Emulator is a shared library containing functionality to locally emulate blockchain transactions.


To emulate transaction you need the following data:

- Account state of type *ShardAccount*.
- Global config of type *(Hashmap 32 ^Cell)*.
- Inbound message of type *MessageAny*.

Emulators output is transaction object of type *Transaction*. Also it's possible to get new account state (*ShardAccount*).
