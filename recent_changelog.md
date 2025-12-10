## 2025.12 Update

1. `celldb-v2` enabled by default.
2. Fast state serializer (sharded serialization) enabled by default.
3. Using `_` instead of `:` in package filenames for better compatibility with Windows and network protocols.
4. Introduced parallelism in the validator engine.
5. Added Python-based testing framework.
6. Various fixes in emulator, node, TVM, `Asm.fif`.
7. Introduced improved network traffic compression.
8. BLST updated.

Internal code changes:

1. Introduced support for coroutines in actors.
2. Enabled `clang-format` 21.
3. Removed the virtualization level concept.
4. Minor code style and cleanliness improvements.

Besides the work of the core team, this update also includes contributions from: @pyAndr3w (`Asm.fif`), @Gusarich (TVM fixes in `arithops`), @Kaladin13 (node fixes).
