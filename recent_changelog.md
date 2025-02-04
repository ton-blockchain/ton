## 2025.02 Update

### Core Improvements & Fixes
1. Multiple improvements and fixes for `Config8.version >= 9` – see [GlobalVersions.md](./doc/GlobalVersions.md) for details.
2. Enhanced discovery of updated nodes' (validators') IPs by implementing DHT query retries.
3. Various improvements for extra currency adoption, including:
   - Fix for `c7` in `rungetmethod`
   - Enhancements in reserve modes
4. **TVM Updates**: Improved processing of continuation control data on deep jumps.
5. **TL-B Scheme Fixes**:
   - CRC computation corrections
   - Fix for incorrect tag usage in Merkle proofs
   - Improvements to `advance_ext` and `NatWidth` printing
6. **Emulator Enhancements**:
   - Fixed issues with setting libraries
   - Improved extra currency support
7. Increased gas limit to unlock **highload-v2 wallets** that were locked in early 2024.
8. **Validator Console Improvements**:
   - Enhanced formatting for shard displays
   - Support for dashed names

### Contributors
This update incorporates contributions from the core team and community members, including:
- **@dbaranovstonfi** from StonFi – **Libraries in the emulator**
- **@Rexagon** – **RET improvements on deep jumps**
- **@tvorogme** from DTon – **Fixes for `advance_ext`**
- **Nan** from Zellic – **Enhancements for `stk_und` and JNI**
