#include <algorithm>

#include "common/checksum.h"
#include "td/utils/HashSet.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/bits.h"
#include "td/utils/filesystem.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "vm/boc.h"
#include "vm/cells/DataCell.h"
#include "vm/db/CellStorage.h"

#include "common.h"

namespace bench {

namespace {

int u128_bytes(Uint128 value) {
  int len = 0;
  while (value > 0) {
    ++len;
    value >>= 8;
  }
  return len;
}

void store_uint_be(vm::CellBuilder &cb, Uint128 value, int bytes) {
  if (bytes > 8) {
    cb.store_long(static_cast<long long>(static_cast<td::uint64>(value >> 64)), (bytes - 8) * 8);
    bytes = 8;
    value &= ~static_cast<Uint128>(0) >> 64;
  }
  cb.store_long(static_cast<long long>(static_cast<td::uint64>(value)), bytes * 8);
}

}  // namespace

std::string u128_to_dec(Uint128 value) {
  if (value == 0) {
    return "0";
  }
  std::string s;
  while (value > 0) {
    s.push_back(static_cast<char>('0' + static_cast<int>(value % 10)));
    value /= 10;
  }
  std::reverse(s.begin(), s.end());
  return s;
}

td::Result<Uint128> dec_to_u128(td::Slice s) {
  if (s.empty()) {
    return td::Status::Error("empty decimal number");
  }
  Uint128 value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') {
      return td::Status::Error("invalid decimal number");
    }
    Uint128 next = value * 10 + static_cast<unsigned>(c - '0');
    if (next < value) {
      return td::Status::Error("decimal number overflow");
    }
    value = next;
  }
  return value;
}

// ---------------------------------------------------------------------------
// Contracts
// ---------------------------------------------------------------------------

static td::Result<Ref<vm::DataCell>> load_boc_file(std::string path) {
  TRY_RESULT(data, td::read_file(path));
  TRY_RESULT(cell, vm::std_boc_deserialize(data));
  TRY_RESULT(loaded, cell->load_cell());
  return std::move(loaded.data_cell);
}

td::Result<ContractSet> load_contracts(td::CSlice dir) {
  ContractSet res;
  TRY_RESULT_ASSIGN(res.w5_code, load_boc_file(PSTRING() << dir << "/wallet-v5.code.boc"));
  TRY_RESULT_ASSIGN(res.jw_code, load_boc_file(PSTRING() << dir << "/jetton-wallet.code.boc"));
  TRY_RESULT_ASSIGN(res.minter_code, load_boc_file(PSTRING() << dir << "/jetton-minter.code.boc"));
  return res;
}

// ---------------------------------------------------------------------------
// Derivation
// ---------------------------------------------------------------------------

td::Bits256 tagged_sha256(const td::Bits256 &seed, td::Slice tag, td::uint64 index) {
  char buf[32 + 16 + 8];
  CHECK(tag.size() <= 16);
  td::MutableSlice msg(buf, 32 + tag.size() + 8);
  msg.copy_from(seed.as_slice());
  msg.substr(32).copy_from(tag);
  char idx[8];
  for (int i = 0; i < 8; i++) {
    idx[i] = static_cast<char>((index >> (8 * i)) & 0xff);
  }
  msg.substr(32 + tag.size()).copy_from(td::Slice(idx, 8));
  return td::sha256_bits256(msg);
}

td::Result<td::Ed25519::PrivateKey> derive_w5_private_key(const td::Bits256 &seed, td::uint64 index) {
  auto key_seed = tagged_sha256(seed, "w5", index);
  return td::Ed25519::PrivateKey{td::SecureString(key_seed.as_slice())};
}

td::Result<td::Bits256> derive_w5_pubkey(const td::Bits256 &seed, td::uint64 index) {
  TRY_RESULT(pk, derive_w5_private_key(seed, index));
  TRY_RESULT(pub, pk.get_public_key());
  td::Bits256 res;
  res.as_slice().copy_from(pub.as_octet_string());
  return res;
}

// ---------------------------------------------------------------------------
// TLB helpers
// ---------------------------------------------------------------------------

void store_grams(vm::CellBuilder &cb, Uint128 value) {
  int len = u128_bytes(value);
  CHECK(len < 16);
  cb.store_long(len, 4);
  store_uint_be(cb, value, len);
}

void store_var_uint7(vm::CellBuilder &cb, td::uint64 value) {
  int len = u128_bytes(value);
  CHECK(len < 7);
  cb.store_long(len, 3);
  store_uint_be(cb, value, len);
}

void store_addr_std(vm::CellBuilder &cb, const td::Bits256 &addr) {
  cb.store_long(0b100, 3);  // addr_std$10 anycast:nothing$0
  cb.store_long(0, 8);      // workchain_id:int8 = 0
  cb.store_bits(addr.bits(), 256);
}

void store_addr_none(vm::CellBuilder &cb) {
  cb.store_long(0, 2);
}

void store_currency_collection(vm::CellBuilder &cb, Uint128 grams) {
  store_grams(cb, grams);
  cb.store_long(0, 1);  // other:ExtraCurrencyCollection (empty dict)
}

void store_depth_balance(vm::CellBuilder &cb, Uint128 balance) {
  cb.store_long(0, 5);  // fixed_prefix_length:(#<= 30) = 0
  store_currency_collection(cb, balance);
}

// Byte-identical to vm/dict.cpp append_dict_label{,_same}().
static void append_dict_label_same(vm::CellBuilder &cb, bool same, int len, int max_len) {
  int k = 32 - td::count_leading_zeroes32(max_len);
  CHECK(len >= 0 && len <= max_len && max_len <= 1023);
  if (len > 1 && k < 2 * len - 1) {
    cb.store_long(6 + same, 3).store_long(len, k);  // hml_same$11
  } else if (k < len) {
    cb.store_long(2, 2).store_long(len, k).store_long(-static_cast<int>(same), len);  // hml_long$10
  } else {
    cb.store_long(0, 1).store_long(-2, len + 1).store_long(-static_cast<int>(same), len);  // hml_short$0
  }
}

void append_dict_label(vm::CellBuilder &cb, td::ConstBitPtr label, int len, int max_len) {
  CHECK(len <= max_len && max_len <= 1023);
  if (len > 0 && static_cast<int>(td::bitstring::bits_memscan(label, len, *label)) == len) {
    return append_dict_label_same(cb, *label, len, max_len);
  }
  int k = 32 - td::count_leading_zeroes32(max_len);
  if (k < len) {
    cb.store_long(2, 2).store_long(len, k);  // hml_long$10
  } else {
    cb.store_long(0, 1).store_long(-2, len + 1);  // hml_short$0
  }
  CHECK(static_cast<int>(cb.remaining_bits()) >= len);
  cb.store_bits(label, len);
}

// ---------------------------------------------------------------------------
// Account cell builders
// ---------------------------------------------------------------------------

Ref<vm::DataCell> build_w5_data(const td::Bits256 &pubkey, td::uint32 wallet_id) {
  vm::CellBuilder cb;
  cb.store_long(1, 1);           // is_signature_allowed
  cb.store_long(0, 32);          // seqno
  cb.store_long(wallet_id, 32);  // wallet_id
  cb.store_bits(pubkey.bits(), 256);
  cb.store_long(0, 1);  // extensions:(HashmapE 256 int1)
  return cb.finalize_novm();
}

Ref<vm::DataCell> build_jw_data(Uint128 jetton_balance, const td::Bits256 &owner_addr, const td::Bits256 &minter_addr,
                                Ref<vm::Cell> jw_code) {
  vm::CellBuilder cb;
  store_grams(cb, jetton_balance);
  store_addr_std(cb, owner_addr);
  store_addr_std(cb, minter_addr);
  cb.store_ref(std::move(jw_code));
  return cb.finalize_novm();
}

Ref<vm::DataCell> build_minter_data(Uint128 total_supply, Ref<vm::Cell> content, Ref<vm::Cell> jw_code) {
  vm::CellBuilder cb;
  store_grams(cb, total_supply);
  store_addr_none(cb);  // admin_address
  cb.store_ref(std::move(content));
  cb.store_ref(std::move(jw_code));
  return cb.finalize_novm();
}

Ref<vm::DataCell> build_state_init(Ref<vm::Cell> code, Ref<vm::Cell> data) {
  vm::CellBuilder cb;
  cb.store_long(0b00110, 5);  // no fixed_prefix_length, no special, code, data, no library
  cb.store_ref(std::move(code));
  cb.store_ref(std::move(data));
  return cb.finalize_novm();
}

std::vector<Ref<vm::DataCell>> build_ballast_chain(const td::Bits256 &addr, int num_cells) {
  CHECK(num_cells >= 1);
  std::vector<Ref<vm::DataCell>> cells(num_cells);
  td::uint64 addr64 = 0;
  for (int i = 0; i < 8; i++) {
    addr64 |= static_cast<td::uint64>(addr.as_slice().ubegin()[i]) << (8 * i);
  }
  for (int k = num_cells - 1; k >= 0; --k) {
    td::uint64 state = addr64 ^ (0xB10A57C811u * static_cast<td::uint64>(k + 1));
    unsigned char filler[127];
    for (int p = 0; p < 127; p += 8) {
      td::uint64 word = splitmix64_next(state);
      for (int b = 0; b < 8 && p + b < 127; b++) {
        filler[p + b] = static_cast<unsigned char>(word >> (8 * b));
      }
    }
    vm::CellBuilder cb;
    cb.store_bits(filler, 127 * 8);
    if (k + 1 < num_cells) {
      cb.store_ref(cells[k + 1]);
    }
    cells[k] = cb.finalize_novm();
  }
  return cells;
}

Ref<vm::DataCell> build_ballast_code() {
  vm::CellBuilder cb;
  cb.store_bytes(td::Slice("tonbench-ballast"));
  return cb.finalize_novm();
}

Ref<vm::DataCell> build_empty_cell() {
  return vm::CellBuilder{}.finalize_novm();
}

// bits of the AccountStorage "root" (the part serialized inline in the Account cell)
static td::uint32 account_storage_root_bits(Uint128 balance) {
  // last_trans_lt:uint64 balance:CurrencyCollection state:account_active$1 StateInit(5 bits)
  return 64 + 4 + 8 * static_cast<td::uint32>(u128_bytes(balance)) + 1 + 1 + 5;
}

StorageUsedStat compute_account_storage_used(Uint128 balance, td::Span<Ref<vm::Cell>> state_roots) {
  StorageUsedStat res;
  res.cells = 1;  // the AccountStorage root itself
  res.bits = account_storage_root_bits(balance);
  td::HashSet<vm::CellHash> visited;
  std::vector<Ref<vm::Cell>> stack(state_roots.begin(), state_roots.end());
  while (!stack.empty()) {
    auto cell = std::move(stack.back());
    stack.pop_back();
    if (!visited.insert(cell->get_hash()).second) {
      continue;
    }
    auto loaded = cell->load_cell().move_as_ok();
    res.cells++;
    res.bits += loaded.data_cell->get_bits();
    for (unsigned i = 0; i < loaded.data_cell->get_refs_cnt(); i++) {
      stack.push_back(loaded.data_cell->get_ref(i));
    }
  }
  return res;
}

Ref<vm::DataCell> build_account(const td::Bits256 &addr, Uint128 balance, Ref<vm::Cell> code, Ref<vm::Cell> data,
                                const StorageUsedStat &used, td::uint32 last_paid) {
  vm::CellBuilder cb;
  cb.store_long(1, 1);  // account$1
  store_addr_std(cb, addr);
  // storage_stat:StorageInfo
  store_var_uint7(cb, used.cells);
  store_var_uint7(cb, used.bits);
  cb.store_long(0, 3);           // storage_extra_none$000
  cb.store_long(last_paid, 32);  // last_paid
  cb.store_long(0, 1);           // due_payment:nothing
  // storage:AccountStorage
  cb.store_long(0, 64);  // last_trans_lt
  store_currency_collection(cb, balance);
  cb.store_long(1, 1);        // account_active$1
  cb.store_long(0b00110, 5);  // StateInit{code,data}
  cb.store_ref(std::move(code));
  cb.store_ref(std::move(data));
  return cb.finalize_novm();
}

// ---------------------------------------------------------------------------
// Stand-ins + subtree emission
// ---------------------------------------------------------------------------

Ref<vm::Cell> make_standin(const Ref<vm::Cell> &cell) {
  CHECK(cell->get_level() == 0);
  auto hash = cell->get_hash();
  unsigned char depth_buf[vm::Cell::depth_bytes];
  vm::DataCell::store_depth(depth_buf, cell->get_depth());
  vm::PrunnedCellInfo info{vm::Cell::LevelMask{0}, hash.as_slice(), td::Slice(depth_buf, vm::Cell::depth_bytes)};
  return StandinCell::create(info, StandinExtra{}).move_as_ok();
}

void emit_subtree(CellSink &sink, const Ref<vm::Cell> &root) {
  td::HashSet<vm::CellHash> visited;
  std::vector<Ref<vm::Cell>> stack{root};
  while (!stack.empty()) {
    auto cell = std::move(stack.back());
    stack.pop_back();
    if (!visited.insert(cell->get_hash()).second) {
      continue;
    }
    auto loaded = cell->load_cell().move_as_ok();
    sink.emit(loaded.data_cell);
    for (unsigned i = 0; i < loaded.data_cell->get_refs_cnt(); i++) {
      stack.push_back(loaded.data_cell->get_ref(i));
    }
  }
}

// ---------------------------------------------------------------------------
// Streaming ShardAccounts builder
// ---------------------------------------------------------------------------

Ref<vm::DataCell> materialize_dict_node(CellSink &sink, const DictNode &node, int edge_start) {
  CHECK(node.type != DictNode::Type::Empty);
  vm::CellBuilder cb;
  if (node.type == DictNode::Type::Leaf) {
    append_dict_label(cb, node.key.bits() + edge_start, 256 - edge_start, 256 - edge_start);
    store_depth_balance(cb, node.balance);  // extra
    cb.store_ref(node.account);             // account:^Account
    cb.store_bits_same(256, false);         // last_trans_hash
    cb.store_long(0, 64);                   // last_trans_lt
  } else {
    CHECK(edge_start <= node.fork_pos && node.fork_pos < 256);
    append_dict_label(cb, node.key.bits() + edge_start, node.fork_pos - edge_start, 256 - edge_start);
    cb.store_ref(node.left);
    cb.store_ref(node.right);
    store_depth_balance(cb, node.balance);  // extra
  }
  auto cell = cb.finalize_novm();
  sink.emit(cell);
  return cell;
}

void ShardAccountsStreamBuilder::add_account(const td::Bits256 &addr, Ref<vm::Cell> account_cell, Uint128 balance) {
  DictNode node;
  node.type = DictNode::Type::Leaf;
  node.key = addr;
  node.balance = balance;
  node.account = std::move(account_cell);
  add_node(std::move(node));
}

void ShardAccountsStreamBuilder::add_subtree(DictNode node) {
  CHECK(node.type != DictNode::Type::Empty);
  add_node(std::move(node));
}

void ShardAccountsStreamBuilder::add_node(DictNode node) {
  if (carry_.type == DictNode::Type::Empty) {
    CHECK(stack_.empty());
    carry_ = std::move(node);
    return;
  }
  std::size_t lcp = 0;
  td::bitstring::bits_memcmp(carry_.key.bits(), node.key.bits(), 256, &lcp);
  int fork_pos = static_cast<int>(lcp);
  CHECK(fork_pos < 256);                                             // keys must be distinct
  CHECK(!carry_.key.bits()[fork_pos] && node.key.bits()[fork_pos]);  // strictly increasing
  CHECK(node.type != DictNode::Type::Fork || fork_pos < node.fork_pos);
  CHECK(carry_.type != DictNode::Type::Fork || fork_pos < carry_.fork_pos);
  // Close all pending forks deeper than the new fork.
  while (!stack_.empty() && stack_.back().pos > fork_pos) {
    auto fork = std::move(stack_.back());
    stack_.pop_back();
    CHECK(fork.pos != fork_pos);
    auto right = materialize_dict_node(sink_, carry_, fork.pos + 1);
    DictNode merged;
    merged.type = DictNode::Type::Fork;
    merged.key = carry_.key;
    merged.balance = fork.left_balance + carry_.balance;
    merged.fork_pos = fork.pos;
    merged.left = std::move(fork.left);
    merged.right = make_standin(right);
    carry_ = std::move(merged);
  }
  // Open the new fork: its left child is now complete.
  auto left = materialize_dict_node(sink_, carry_, fork_pos + 1);
  stack_.push_back(OpenFork{fork_pos, make_standin(left), carry_.balance});
  carry_ = std::move(node);
}

DictNode ShardAccountsStreamBuilder::finish() {
  if (carry_.type == DictNode::Type::Empty) {
    CHECK(stack_.empty());
    return {};
  }
  while (!stack_.empty()) {
    auto fork = std::move(stack_.back());
    stack_.pop_back();
    auto right = materialize_dict_node(sink_, carry_, fork.pos + 1);
    DictNode merged;
    merged.type = DictNode::Type::Fork;
    merged.key = carry_.key;
    merged.balance = fork.left_balance + carry_.balance;
    merged.fork_pos = fork.pos;
    merged.left = std::move(fork.left);
    merged.right = make_standin(right);
    carry_ = std::move(merged);
  }
  return std::move(carry_);
}

// ---------------------------------------------------------------------------
// Shard state assembly (mirrors tontester zerostate.py base_state)
// ---------------------------------------------------------------------------

Ref<vm::DataCell> build_shard_accounts_cell(const Ref<vm::Cell> &accounts_dict_root, Uint128 total_balance) {
  vm::CellBuilder cb;
  if (accounts_dict_root.is_null()) {
    CHECK(total_balance == 0);
    cb.store_long(0, 1);  // ahme_empty$0
  } else {
    cb.store_long(1, 1);  // ahme_root$1
    cb.store_ref(accounts_dict_root);
  }
  store_depth_balance(cb, total_balance);  // extra:DepthBalanceInfo
  return cb.finalize_novm();
}

Ref<vm::DataCell> build_shard_state_root(CellSink &sink, const Ref<vm::Cell> &accounts_dict_root, Uint128 total_balance,
                                         td::uint32 gen_utime) {
  // out_msg_queue_info:^OutMsgQueueInfo — everything empty
  vm::CellBuilder qb;
  qb.store_long(0, 1);   // out_queue:OutMsgQueue = ahme_empty$0
  qb.store_long(0, 64);  // ... extra:uint64 = 0
  qb.store_long(0, 1);   // proc_info:ProcessedInfo (empty HashmapE)
  qb.store_long(0, 1);   // extra:(Maybe OutMsgQueueExtra) = nothing
  auto out_msg_queue_info = qb.finalize_novm();
  sink.emit(out_msg_queue_info);

  auto accounts = build_shard_accounts_cell(accounts_dict_root, total_balance);
  sink.emit(accounts);

  // ^[ overload_history underload_history total_balance total_validator_fees libraries master_ref ]
  vm::CellBuilder eb;
  eb.store_long(0, 64);  // overload_history
  eb.store_long(0, 64);  // underload_history
  store_currency_collection(eb, total_balance);
  store_currency_collection(eb, 0);  // total_validator_fees
  eb.store_long(0, 1);               // libraries:(HashmapE 256 LibDescr)
  eb.store_long(0, 1);               // master_ref:(Maybe BlkMasterInfo)
  auto extra = eb.finalize_novm();
  sink.emit(extra);

  vm::CellBuilder cb;
  cb.store_long(0x9023afe2, 32);  // shard_state#9023afe2
  cb.store_long(kGlobalId, 32);   // global_id:int32
  // shard_id:ShardIdent — shard_ident$00 pfx_bits=0 wc=0 prefix=1<<63
  // (raw prefix 1<<63 matches tontester zerostate.py; the validator's
  //  block::ShardId::deserialize normalizes it to the same shard)
  cb.store_long(0, 2);
  cb.store_long(0, 6);
  cb.store_long(0, 32);
  cb.store_long(static_cast<long long>(1ULL << 63), 64);
  cb.store_long(0, 32);             // seq_no
  cb.store_long(0, 32);             // vert_seq_no
  cb.store_long(gen_utime, 32);     // gen_utime
  cb.store_long(0, 64);             // gen_lt
  cb.store_long(0xFFFFFFFFLL, 32);  // min_ref_mc_seqno
  cb.store_ref(out_msg_queue_info);
  cb.store_long(0, 1);  // before_split
  cb.store_ref(accounts);
  cb.store_ref(extra);
  cb.store_long(0, 1);  // custom:(Maybe ^McStateExtra) = nothing
  auto root = cb.finalize_novm();
  sink.emit(root);
  return root;
}

td::Bits256 fake_file_hash(const td::Bits256 &root_hash) {
  char buf[18 + 32];
  td::MutableSlice msg(buf, sizeof(buf));
  msg.copy_from(td::Slice("tonbench-fake-file"));
  msg.substr(18).copy_from(root_hash.as_slice());
  return td::sha256_bits256(msg);
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

std::string Manifest::to_json() const {
  td::JsonBuilder jb;
  auto obj = jb.enter_object();
  obj("version", version);
  obj("seed_hex", td::hex_encode(seed.as_slice()));
  obj("global_id", global_id);
  obj("gen_utime", static_cast<td::int64>(gen_utime));
  obj("root_hash_hex", td::hex_encode(root_hash.as_slice()));
  obj("file_hash_hex", td::hex_encode(file_hash.as_slice()));
  obj("total_balance", u128_to_dec(total_balance));
  obj("num_v5", static_cast<td::int64>(num_v5));
  obj("num_ballast", static_cast<td::int64>(num_ballast));
  obj("ballast_cells", ballast_cells);
  obj("wallet_id", static_cast<td::int64>(wallet_id));
  obj("w5_code_hash_hex", td::hex_encode(w5_code_hash.as_slice()));
  obj("jw_code_hash_hex", td::hex_encode(jw_code_hash.as_slice()));
  obj("minter_addr_hex", td::hex_encode(minter_addr.as_slice()));
  obj("v5_balance", u128_to_dec(v5_balance));
  obj("jw_balance", u128_to_dec(jw_balance));
  obj("jw_jetton_balance", u128_to_dec(jw_jetton_balance));
  obj("celldb_path", celldb_path);
  obj.leave();
  return jb.string_builder().as_cslice().str();
}

static td::Result<td::Bits256> bits256_from_hex(td::Slice hex) {
  TRY_RESULT(data, td::hex_decode(hex));
  if (data.size() != 32) {
    return td::Status::Error("expected 32 hex-encoded bytes");
  }
  td::Bits256 res;
  res.as_slice().copy_from(data);
  return res;
}

td::Result<Manifest> Manifest::from_json(td::Slice json) {
  std::string copy = json.str();
  TRY_RESULT(value, td::json_decode(copy));
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("manifest: expected JSON object");
  }
  auto &obj = value.get_object();
  Manifest m;
  TRY_RESULT_ASSIGN(m.version, obj.get_required_int_field("version"));
  if (m.version != 1) {
    return td::Status::Error("manifest: unsupported version");
  }
  TRY_RESULT(seed_hex, obj.get_required_string_field("seed_hex"));
  TRY_RESULT_ASSIGN(m.seed, bits256_from_hex(seed_hex));
  TRY_RESULT_ASSIGN(m.global_id, obj.get_required_int_field("global_id"));
  TRY_RESULT(gen_utime, obj.get_required_long_field("gen_utime"));
  m.gen_utime = static_cast<td::uint32>(gen_utime);
  TRY_RESULT(root_hash_hex, obj.get_required_string_field("root_hash_hex"));
  TRY_RESULT_ASSIGN(m.root_hash, bits256_from_hex(root_hash_hex));
  TRY_RESULT(file_hash_hex, obj.get_required_string_field("file_hash_hex"));
  TRY_RESULT_ASSIGN(m.file_hash, bits256_from_hex(file_hash_hex));
  TRY_RESULT(total_balance, obj.get_required_string_field("total_balance"));
  TRY_RESULT_ASSIGN(m.total_balance, dec_to_u128(total_balance));
  TRY_RESULT(num_v5, obj.get_required_long_field("num_v5"));
  m.num_v5 = static_cast<td::uint64>(num_v5);
  TRY_RESULT(num_ballast, obj.get_required_long_field("num_ballast"));
  m.num_ballast = static_cast<td::uint64>(num_ballast);
  TRY_RESULT_ASSIGN(m.ballast_cells, obj.get_required_int_field("ballast_cells"));
  TRY_RESULT(wallet_id, obj.get_required_long_field("wallet_id"));
  m.wallet_id = static_cast<td::uint32>(wallet_id);
  TRY_RESULT(w5_code_hash_hex, obj.get_required_string_field("w5_code_hash_hex"));
  TRY_RESULT_ASSIGN(m.w5_code_hash, bits256_from_hex(w5_code_hash_hex));
  TRY_RESULT(jw_code_hash_hex, obj.get_required_string_field("jw_code_hash_hex"));
  TRY_RESULT_ASSIGN(m.jw_code_hash, bits256_from_hex(jw_code_hash_hex));
  TRY_RESULT(minter_addr_hex, obj.get_required_string_field("minter_addr_hex"));
  TRY_RESULT_ASSIGN(m.minter_addr, bits256_from_hex(minter_addr_hex));
  TRY_RESULT(v5_balance, obj.get_required_string_field("v5_balance"));
  TRY_RESULT_ASSIGN(m.v5_balance, dec_to_u128(v5_balance));
  TRY_RESULT(jw_balance, obj.get_required_string_field("jw_balance"));
  TRY_RESULT_ASSIGN(m.jw_balance, dec_to_u128(jw_balance));
  TRY_RESULT(jw_jetton_balance, obj.get_required_string_field("jw_jetton_balance"));
  TRY_RESULT_ASSIGN(m.jw_jetton_balance, dec_to_u128(jw_jetton_balance));
  TRY_RESULT_ASSIGN(m.celldb_path, obj.get_required_string_field("celldb_path"));
  return m;
}

// ---------------------------------------------------------------------------
// Wallet info + spam external
// ---------------------------------------------------------------------------

td::Result<WalletInfo> derive_wallet(const td::Bits256 &seed, td::uint64 index, td::uint32 wallet_id,
                                     const td::Bits256 &minter_addr, const ContractSet &contracts) {
  WalletInfo info;
  TRY_RESULT_ASSIGN(info.pubkey, derive_w5_pubkey(seed, index));
  auto w5_data = build_w5_data(info.pubkey, wallet_id);
  info.w5_addr = build_state_init(contracts.w5_code, w5_data)->get_hash().bits();
  // Jetton wallet address is derived from the *zero-balance* initial data, as
  // the jetton contracts do in calculate_user_jetton_wallet_address().
  auto jw_data0 = build_jw_data(0, info.w5_addr, minter_addr, contracts.jw_code);
  info.jw_addr = build_state_init(contracts.jw_code, jw_data0)->get_hash().bits();
  return info;
}

td::Result<Ref<vm::DataCell>> build_signed_external(const td::Bits256 &seed, td::uint64 wallet_index,
                                                    td::uint64 recipient_index, const Manifest &manifest,
                                                    const ContractSet &contracts, const SpamParams &params) {
  TRY_RESULT(sender, derive_wallet(seed, wallet_index, manifest.wallet_id, manifest.minter_addr, contracts));
  TRY_RESULT(recipient, derive_wallet(seed, recipient_index, manifest.wallet_id, manifest.minter_addr, contracts));

  // Jetton transfer body (op-codes.fc op::transfer)
  vm::CellBuilder body;
  body.store_long(0xf8a7ea5, 32);                             // op
  body.store_long(static_cast<long long>(wallet_index), 64);  // query_id
  store_grams(body, params.jetton_amount);                    // amount
  store_addr_std(body, recipient.w5_addr);                    // destination (new owner)
  store_addr_none(body);                                      // response_destination
  body.store_long(0, 1);                                      // custom_payload:(Maybe ^Cell)
  store_grams(body, 0);                                       // forward_ton_amount
  body.store_long(0, 1);                                      // forward_payload:(Either Cell ^Cell) = inline empty

  // MessageRelaxed: internal to the sender's jetton wallet
  vm::CellBuilder msg;
  msg.store_long(0, 1);  // int_msg_info$0
  msg.store_long(1, 1);  // ihr_disabled
  msg.store_long(1, 1);  // bounce
  msg.store_long(0, 1);  // bounced
  store_addr_none(msg);  // src
  store_addr_std(msg, sender.jw_addr);
  store_currency_collection(msg, params.msg_value);
  store_grams(msg, 0);    // ihr_fee
  store_grams(msg, 0);    // fwd_fee
  msg.store_long(0, 64);  // created_lt
  msg.store_long(0, 32);  // created_at
  msg.store_long(0, 1);   // init:(Maybe ...)
  msg.store_long(0, 1);   // body:(Either X ^X) = inline
  msg.append_builder(body);
  auto msg_cell = msg.finalize_novm();

  // c5 / OutList: out_list$_ prev:^(OutList 0) action:(action_send_msg mode)
  vm::CellBuilder c5;
  c5.store_ref(build_empty_cell());  // out_list_empty$_
  c5.store_long(0x0ec3c86d, 32);     // action_send_msg
  c5.store_long(3, 8);               // mode = +1 pay fees separately, +2 ignore errors
  c5.store_ref(msg_cell);
  auto c5_cell = c5.finalize_novm();

  // Signed wallet-v5 external body
  vm::CellBuilder inner;
  inner.store_long(0x7369676E, 32);          // "sign"
  inner.store_long(manifest.wallet_id, 32);  // wallet_id
  inner.store_long(0xFFFFFFFELL, 32);        // valid_until
  inner.store_long(0, 32);                   // seqno
  inner.store_long(1, 1);                    // (Maybe OutList) = just
  inner.store_ref(c5_cell);
  inner.store_long(0, 1);  // no other actions
  auto unsigned_cell = inner.finalize_copy();

  TRY_RESULT(pk, derive_w5_private_key(seed, wallet_index));
  TRY_RESULT(sig, pk.sign(unsigned_cell->get_hash().as_slice()));
  CHECK(sig.size() == 64);
  inner.store_bytes(sig.as_slice());

  // ext_in_msg_info$10 src:addr_none dest:(addr_std wallet) import_fee:0; no init; inline body
  vm::CellBuilder ext;
  ext.store_long(0b10, 2);
  store_addr_none(ext);
  store_addr_std(ext, sender.w5_addr);
  store_grams(ext, 0);   // import_fee
  ext.store_long(0, 1);  // init:(Maybe ...)
  ext.store_long(0, 1);  // body:(Either X ^X) = inline
  ext.append_builder(inner);
  return ext.finalize_novm();
}

}  // namespace bench
