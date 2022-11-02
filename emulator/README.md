# Transaction Emulator

Transaction Emulator is a shared library containing functionality to locally emulate blockchain transactions.


To emulate a transaction you need the following data:

- Account state of type *ShardAccount*.
- Global config of type *(Hashmap 32 ^Cell)*.
- Inbound message of type *MessageAny*.

Emulator output is transaction object (*Transaction*), new account state (*ShardAccount*), TVM log.

When emulating transactions keep in mind the following:
- *chksig* instructions are set to always succeed in case of an external message.
- *lt* is set as the transaction happens in the block next after the account's last transaction block.
- *utime* is set as the current system time.
