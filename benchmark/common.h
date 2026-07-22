/*
    bench-state-gen / bench-spam shared helpers.  See benchmark/DESIGN.md.

    Everything here is deliberately deterministic: all key material and all
    synthesized cells are derived from a single 32-byte seed.
*/
#pragma once

#include <string>
#include <vector>

#include "common/bitstring.h"
#include "td/utils/HashMap.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/common.h"
#include "vm/cells.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/ExtCell.h"

#include "Ed25519.h"

namespace bench {

using td::Ref;
// Balances: total state balance (~8.1e18 nanoton at full scale) is close to the
// uint64 range, jetton supply exceeds it; use a 128-bit type everywhere.
using Uint128 = unsigned __int128;

std::string u128_to_dec(Uint128 value);
td::Result<Uint128> dec_to_u128(td::Slice s);

// ---------------------------------------------------------------------------
// Contract code artifacts (benchmark/contracts/*.boc)
// ---------------------------------------------------------------------------

struct ContractSet {
  Ref<vm::DataCell> w5_code;
  Ref<vm::DataCell> jw_code;
  Ref<vm::DataCell> minter_code;
};

td::Result<ContractSet> load_contracts(td::CSlice dir);

// ---------------------------------------------------------------------------
// Deterministic derivation
// ---------------------------------------------------------------------------

// SHA256(seed || tag || u64le(index))
td::Bits256 tagged_sha256(const td::Bits256 &seed, td::Slice tag, td::uint64 index);

// ed25519 private key for wallet-v5 #index; seed32 = SHA256(seed || "w5" || u64le(index))
td::Result<td::Ed25519::PrivateKey> derive_w5_private_key(const td::Bits256 &seed, td::uint64 index);
td::Result<td::Bits256> derive_w5_pubkey(const td::Bits256 &seed, td::uint64 index);

inline td::uint64 splitmix64_next(td::uint64 &x) {
  x += 0x9E3779B97F4A7C15ULL;
  td::uint64 z = x;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// ---------------------------------------------------------------------------
// Low-level TLB serialization helpers
// ---------------------------------------------------------------------------

// Grams = VarUInteger 16 (canonical, minimal length)
void store_grams(vm::CellBuilder &cb, Uint128 value);
// VarUInteger 7 (canonical) — for StorageUsed fields
void store_var_uint7(vm::CellBuilder &cb, td::uint64 value);
// addr_std$10 anycast:nothing workchain:0 address
void store_addr_std(vm::CellBuilder &cb, const td::Bits256 &addr);
// addr_none$00
void store_addr_none(vm::CellBuilder &cb);
// DepthBalanceInfo with fixed_prefix_length = 0 and empty extra currencies
void store_depth_balance(vm::CellBuilder &cb, Uint128 balance);
// CurrencyCollection with empty extra currencies
void store_currency_collection(vm::CellBuilder &cb, Uint128 grams);
// Hashmap edge label (hml_short$0 / hml_long$10 / hml_same$11), byte-identical
// to vm/dict.cpp append_dict_label().
void append_dict_label(vm::CellBuilder &cb, td::ConstBitPtr label, int len, int max_len);

// ---------------------------------------------------------------------------
// Account cell builders
// ---------------------------------------------------------------------------

// w5 data: is_signature_allowed=1, seqno=0, wallet_id, pubkey, empty extensions dict
Ref<vm::DataCell> build_w5_data(const td::Bits256 &pubkey, td::uint32 wallet_id);
// jetton-wallet data: balance:Coins owner:MsgAddressInt master:MsgAddressInt code:^Cell
Ref<vm::DataCell> build_jw_data(Uint128 jetton_balance, const td::Bits256 &owner_addr, const td::Bits256 &minter_addr,
                                Ref<vm::Cell> jw_code);
// jetton-minter data: total_supply:Coins admin:MsgAddress(none) content:^Cell code:^Cell
Ref<vm::DataCell> build_minter_data(Uint128 total_supply, Ref<vm::Cell> content, Ref<vm::Cell> jw_code);
// StateInit with code+data only (5 bits, 2 refs); its hash is the account address
Ref<vm::DataCell> build_state_init(Ref<vm::Cell> code, Ref<vm::Cell> data);
// Ballast data: chain of `num_cells` cells, 127 bytes of splitmix64 filler each,
// keyed by (addr, cell index); element 0 is the chain head.
std::vector<Ref<vm::DataCell>> build_ballast_chain(const td::Bits256 &addr, int num_cells);
// Shared trivial ballast code cell
Ref<vm::DataCell> build_ballast_code();
Ref<vm::DataCell> build_empty_cell();

// StorageUsed of one account: cells/bits over the serialized AccountStorage
// tree (the inline AccountStorage root + deduplicated code/data subtrees).
struct StorageUsedStat {
  td::uint64 cells{0};
  td::uint64 bits{0};
};
// state_roots = {code, data} real (fully loadable) cells of a sample account
StorageUsedStat compute_account_storage_used(Uint128 balance, td::Span<Ref<vm::Cell>> state_roots);

// account$1 addr:(addr_std 0,addr) storage_stat storage:(active StateInit{code,data})
Ref<vm::DataCell> build_account(const td::Bits256 &addr, Uint128 balance, Ref<vm::Cell> code, Ref<vm::Cell> data,
                                const StorageUsedStat &used, td::uint32 last_paid);

// ---------------------------------------------------------------------------
// Cell sink + stand-in cells
// ---------------------------------------------------------------------------

class CellSink {
 public:
  virtual ~CellSink() = default;
  virtual void emit(const Ref<vm::DataCell> &cell) = 0;
  // Emit a record with a custom serialized value (e.g. a bundle record) keyed by
  // `hash`. The same hash may also be emitted as a plain cell; the merge phase
  // keeps the bundle value (see merge_runs_to_sst in state-gen.cpp).
  virtual void emit_raw(const td::Bits256 &hash, std::string value) = 0;
};

struct StandinExtra {};
struct StandinLoader {
  static td::Result<Ref<vm::DataCell>> load_data_cell(const vm::Cell &, const StandinExtra &) {
    return td::Status::Error("stand-in cell cannot be loaded");
  }
};
// A cell carrying only hash/depth (level 0); used as a ref placeholder after the
// real subtree has been emitted, so the subtree memory can be freed.
using StandinCell = vm::ExtCell<StandinExtra, StandinLoader>;

Ref<vm::Cell> make_standin(const Ref<vm::Cell> &cell);

inline Ref<vm::Cell> emit_and_standin(CellSink &sink, const Ref<vm::DataCell> &cell) {
  sink.emit(cell);
  return make_standin(cell);
}

// Emit every distinct cell of a real subtree once (DFS, dedup by hash).
void emit_subtree(CellSink &sink, const Ref<vm::Cell> &root);

// ---------------------------------------------------------------------------
// Dictionary-layer bundling (celldb "bundle" records, vm::CellStorer::kBundleTag)
// ---------------------------------------------------------------------------

// Groups the accounts-dict interior into bundle records so that one celldb read
// materializes ~`bits_per_bundle` levels of the dictionary descent (plus, at the
// leaves, the ShardAccount -> Account -> data-root chain). A dict node whose edge
// starts at key bit s belongs to the bit window floor(s / bits_per_bundle); a node
// is a bundle ROOT iff its window differs from its parent's (the dict root always
// is one). Within a bundle everything `retain`ed and reachable is stored inline;
// shared cells (contract code, chained ballast data) are never retained, so they
// stay external hash references.
//
// Usage: retain() every cell that may be inlined (dict nodes are retained by
// materialize_dict_node, account/data cells by the caller); close_out(h) when a
// node is known to be a bundle root — this serializes the bundle, emits it via
// emit_raw and releases the slab. take_retained()/adopt() hand pending cells from
// per-bucket builders to the top-level builder.
class BundleTracker {
 public:
  BundleTracker(CellSink &sink, int bits_per_bundle, td::int32 refcnt)
      : sink_(sink), bits_(bits_per_bundle), refcnt_(refcnt) {
    CHECK(bits_ > 0);
  }
  int window(int bit_pos) const {
    return bit_pos / bits_;
  }
  void retain(const Ref<vm::DataCell> &cell);
  void close_out(const vm::CellHash &hash);
  std::vector<Ref<vm::DataCell>> take_retained();
  void adopt(std::vector<Ref<vm::DataCell>> cells);
  size_t retained_count() const {
    return retained_.size();
  }

 private:
  CellSink &sink_;
  int bits_;
  td::int32 refcnt_;
  td::HashMap<vm::CellHash, Ref<vm::DataCell>> retained_;
};

// ---------------------------------------------------------------------------
// Streaming ShardAccounts (HashmapAug 256 ShardAccount DepthBalanceInfo) builder
// ---------------------------------------------------------------------------

// An unmaterialized dictionary node: everything except the edge label, which
// depends on where the parent fork ends up (label re-rooting).
struct DictNode {
  enum class Type { Empty, Leaf, Fork };
  Type type{Type::Empty};
  td::Bits256 key{};      // Leaf: the key; Fork: any key inside the subtree
  Uint128 balance{0};     // subtree balance (DepthBalanceInfo extra)
  Ref<vm::Cell> account;  // Leaf: ^Account (stand-in ok)
  int fork_pos{0};        // Fork: bit position where left/right diverge
  Ref<vm::Cell> left;     // Fork: child cells (stand-ins)
  Ref<vm::Cell> right;
};

// Store the label of `node` for an edge starting at bit `edge_start`, then the
// node body; emit the resulting cell. Does not emit/alter children.
// With a tracker: retains the cell for bundling and, if this node's children fall
// into the next bit window, closes out their bundles (they are bundle roots).
Ref<vm::DataCell> materialize_dict_node(CellSink &sink, const DictNode &node, int edge_start,
                                        BundleTracker *tracker = nullptr);

// Builds the accounts dictionary from a strictly increasing key stream with
// O(depth) live cells; every finished cell is emitted bottom-up via the sink.
// Augmentation (DepthBalanceInfo) byte-matches block::tlb::aug_ShardAccounts
// for accounts with fixed_prefix_length 0 and no extra currencies.
class ShardAccountsStreamBuilder {
 public:
  explicit ShardAccountsStreamBuilder(CellSink &sink, BundleTracker *tracker = nullptr)
      : sink_(sink), tracker_(tracker) {
  }
  // Leaf value: account_descr$_ account:^Account last_trans_hash:0 last_trans_lt:0
  void add_account(const td::Bits256 &addr, Ref<vm::Cell> account_cell, Uint128 balance);
  // Splice in a complete subtree (e.g. a per-bucket result); all its keys must
  // be greater than every key added so far.
  void add_subtree(DictNode node);
  // Returns the (unmaterialized) root; Type::Empty if no accounts were added.
  DictNode finish();

 private:
  struct OpenFork {
    int pos;
    Ref<vm::Cell> left;
    Uint128 left_balance;
  };
  void add_node(DictNode node);

  CellSink &sink_;
  BundleTracker *tracker_{nullptr};
  std::vector<OpenFork> stack_;
  DictNode carry_;  // most recently completed subtree, label still unknown
};

// ---------------------------------------------------------------------------
// Shard state assembly (mirrors tontester zerostate.py base_state)
// ---------------------------------------------------------------------------

inline constexpr td::int32 kGlobalId = -777;

// ShardAccounts (HashmapAugE wrapper) cell; accounts_dict_root may be null (empty dict).
Ref<vm::DataCell> build_shard_accounts_cell(const Ref<vm::Cell> &accounts_dict_root, Uint128 total_balance);
// Full basechain (wc 0, shard 8000000000000000, seqno 0) state root; emits all
// header cells (incl. the root itself) into the sink.
Ref<vm::DataCell> build_shard_state_root(CellSink &sink, const Ref<vm::Cell> &accounts_dict_root, Uint128 total_balance,
                                         td::uint32 gen_utime);
// fake file hash = SHA256("tonbench-fake-file" || root_hash)
td::Bits256 fake_file_hash(const td::Bits256 &root_hash);

// ---------------------------------------------------------------------------
// Manifest (manifest.json)
// ---------------------------------------------------------------------------

struct Manifest {
  int version{1};
  td::Bits256 seed{};
  td::int32 global_id{kGlobalId};
  td::uint32 gen_utime{0};
  td::Bits256 root_hash{};
  td::Bits256 file_hash{};
  Uint128 total_balance{0};
  td::uint64 num_v5{0};
  td::uint64 num_ballast{0};
  int ballast_cells{17};
  td::uint32 wallet_id{0};
  td::Bits256 w5_code_hash{};
  td::Bits256 jw_code_hash{};
  td::Bits256 minter_addr{};
  Uint128 v5_balance{0};
  Uint128 jw_balance{0};
  Uint128 jw_jetton_balance{0};
  std::string celldb_path;

  std::string to_json() const;
  static td::Result<Manifest> from_json(td::Slice json);
};

// ---------------------------------------------------------------------------
// Wallet info + spam external message (reused by bench-spam)
// ---------------------------------------------------------------------------

struct WalletInfo {
  td::Bits256 pubkey;
  td::Bits256 w5_addr;
  td::Bits256 jw_addr;  // address per jetton convention: zero-balance initial data
};

td::Result<WalletInfo> derive_wallet(const td::Bits256 &seed, td::uint64 index, td::uint32 wallet_id,
                                     const td::Bits256 &minter_addr, const ContractSet &contracts);

struct SpamParams {
  Uint128 msg_value{50'000'000};     // 0.05 TON attached to the transfer
  Uint128 jetton_amount{1'000'000};  // jetton units moved per transfer
};

// Pre-signed wallet-v5 external (seqno 0) doing a jetton transfer from wallet
// `wallet_index` to wallet `recipient_index` (DESIGN.md "Spam transaction shape").
td::Result<Ref<vm::DataCell>> build_signed_external(const td::Bits256 &seed, td::uint64 wallet_index,
                                                    td::uint64 recipient_index, const Manifest &manifest,
                                                    const ContractSet &contracts, const SpamParams &params = {});

}  // namespace bench
