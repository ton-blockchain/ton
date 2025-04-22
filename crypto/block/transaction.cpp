/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "block/transaction.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "crypto/openssl/rand.hpp"
#include "td/utils/bits.h"
#include "td/utils/uint128.h"
#include "ton/ton-shard.h"
#include "vm/vm.h"
#include "td/utils/Timer.h"

namespace {
/**
 * Logger that stores the tail of log messages.
 *
 * @param max_size The size of the buffer. Default is 256.
 */
class StringLoggerTail : public td::LogInterface {
 public:
  explicit StringLoggerTail(size_t max_size = 256) : buf(max_size, '\0') {}

  /**
   * Appends a slice of data to the buffer.
   *
   * @param slice The slice of data to be appended.
   */
  void append(td::CSlice slice) override {
    if (slice.size() > buf.size()) {
      slice.remove_prefix(slice.size() - buf.size());
    }
    while (!slice.empty()) {
      size_t s = std::min(buf.size() - pos, slice.size());
      std::copy(slice.begin(), slice.begin() + s, buf.begin() + pos);
      pos += s;
      if (pos == buf.size()) {
        pos = 0;
        truncated = true;
      }
      slice.remove_prefix(s);
    }
  }

  /**
   * Retrieves the tail of the log.
   *
   * @returns The log as std::string.
   */
  std::string get_log() const {
    if (truncated) {
      std::string res = buf;
      std::rotate(res.begin(), res.begin() + pos, res.end());
      return res;
    } else {
      return buf.substr(0, pos);
    }
  }

 private:
  std::string buf;
  size_t pos = 0;
  bool truncated = false;
};
}

namespace block {
using td::Ref;

/**
 * Looks up a library among public libraries.
 *
 * @param key A constant bit pointer representing the key of the library to lookup.
 *
 * @returns A reference to the library cell if found, null otherwise.
 */
Ref<vm::Cell> ComputePhaseConfig::lookup_library(td::ConstBitPtr key) const {
  return libraries ? vm::lookup_library_in(key, libraries->get_root_cell()) : Ref<vm::Cell>{};
}

/*
 * 
 *   ACCOUNTS
 * 
 */

/**
 * Sets the address of the account.
 *
 * @param wc The workchain ID of the account.
 * @param new_addr The new address of the account.
 *
 * @returns True if the address was successfully set, false otherwise.
 */
bool Account::set_address(ton::WorkchainId wc, td::ConstBitPtr new_addr) {
  workchain = wc;
  addr = new_addr;
  return true;
}

/**
 * Sets the length of anycast prefix length in the account address.
 *
 * @param new_length The new rewrite lingth.
 *
 * @returns True if the length was successfully set, False otherwise.
 */
bool Account::set_addr_rewrite_length(int new_length) {
  if (new_length < 0 || new_length > 30) {
    return false;  // invalid value
  }
  if (addr_rewrite_length_set) {
    return addr_rewrite_length == new_length;
  } else {
    addr_rewrite_length = (unsigned char)new_length;
    addr_rewrite_length_set = true;
    return true;
  }
}

/**
 * Checks if the given addr rewrite length is valid for the Account.
 *
 * @param length The addr rewrite length to be checked.
 *
 * @returns True if the addr rewrite length is valid, False otherwise.
 */
bool Account::check_addr_rewrite_length(int length) const {
  return addr_rewrite_length_set ? (length == addr_rewrite_length) : (length >= 0 && length <= 30);
}

/**
 * Parses anycast data of the account address.
 * 
 * Initializes addr_rewrite.
 *
 * @param cs The cell slice containing partially-parsed account address.
 *
 * @returns True if parsing was successful, false otherwise.
 */
bool Account::parse_maybe_anycast(vm::CellSlice& cs) {
  int t = (int)cs.fetch_ulong(1);
  if (t < 0) {
    return false;
  } else if (!t) {
    return set_addr_rewrite_length(0);
  }
  int depth;
  return cs.fetch_uint_leq(30, depth)                     // anycast_info$_ depth:(#<= 30)
         && depth                                         // { depth >= 1 }
         && cs.fetch_bits_to(addr_rewrite.bits(), depth)  // rewrite_pfx:(bits depth)
         && set_addr_rewrite_length(depth);
}

/**
 * Stores the anycast information to a serialized account address.
 *
 * @param cb The vm::CellBuilder object to store the information in.
 *
 * @returns True if the anycast information was successfully stored, false otherwise.
 */
bool Account::store_maybe_anycast(vm::CellBuilder& cb) const {
  if (!addr_rewrite_length_set || !addr_rewrite_length) {
    return cb.store_bool_bool(false);
  }
  return cb.store_bool_bool(true)                                    // just$1
         && cb.store_uint_leq(30, addr_rewrite_length)                      // depth:(#<= 30)
         && cb.store_bits_bool(addr_rewrite.cbits(), addr_rewrite_length);  // rewrite_pfx:(bits depth)
}

/**
 * Unpacks the address from a given CellSlice.
 *
 * @param addr_cs The CellSlice containing the address.
 *
 * @returns True if the address was successfully unpacked, False otherwise.
 */
bool Account::unpack_address(vm::CellSlice& addr_cs) {
  int addr_tag = block::gen::t_MsgAddressInt.get_tag(addr_cs);
  int new_wc = ton::workchainInvalid;
  switch (addr_tag) {
    case block::gen::MsgAddressInt::addr_std:
      if (!(addr_cs.advance(2) && parse_maybe_anycast(addr_cs) && addr_cs.fetch_int_to(8, new_wc) &&
            addr_cs.fetch_bits_to(addr_orig.bits(), 256) && addr_cs.empty_ext())) {
        return false;
      }
      break;
    case block::gen::MsgAddressInt::addr_var:
      // cannot appear in masterchain / basechain
      return false;
    default:
      return false;
  }
  addr_cs.clear();
  if (new_wc == ton::workchainInvalid) {
    return false;
  }
  if (workchain == ton::workchainInvalid) {
    workchain = new_wc;
    addr = addr_orig;
    addr.bits().copy_from(addr_rewrite.cbits(), addr_rewrite_length);
  } else if (addr_rewrite_length) {
    ton::StdSmcAddress new_addr = addr_orig;
    new_addr.bits().copy_from(addr_rewrite.cbits(), addr_rewrite_length);
    if (new_addr != addr) {
      LOG(ERROR) << "error unpacking account " << workchain << ":" << addr.to_hex()
                 << " : account header contains different address " << new_addr.to_hex() << " (with splitting depth "
                 << (int)addr_rewrite_length << ")";
      return false;
    }
  } else if (addr != addr_orig) {
    LOG(ERROR) << "error unpacking account " << workchain << ":" << addr.to_hex()
               << " : account header contains different address " << addr_orig.to_hex();
    return false;
  }
  if (workchain != new_wc) {
    LOG(ERROR) << "error unpacking account " << workchain << ":" << addr.to_hex()
               << " : account header contains different workchain " << new_wc;
    return false;
  }
  addr_rewrite = addr.bits();  // initialize all 32 bits of addr_rewrite
  if (!addr_rewrite_length) {
    my_addr_exact = my_addr;
  }
  return true;
}

/**
 * Unpacks storage information from a CellSlice.
 * 
 * Storage information is serialized using StorageInfo TLB-scheme.
 *
 * @param cs The CellSlice containing the storage information.
 *
 * @returns True if the unpacking is successful, false otherwise.
 */
bool Account::unpack_storage_info(vm::CellSlice& cs) {
  block::gen::StorageInfo::Record info;
  block::gen::StorageUsed::Record used;
  if (!tlb::unpack_exact(cs, info) || !tlb::csr_unpack(info.used, used)) {
    return false;
  }
  last_paid = info.last_paid;
  if (info.storage_extra.write().fetch_long(3) == 1) {
    info.storage_extra->prefetch_bits_to(storage_dict_hash.value_force());
  } else {
    storage_dict_hash = {};
  }
  if (info.due_payment->prefetch_ulong(1) == 1) {
    vm::CellSlice& cs2 = info.due_payment.write();
    cs2.advance(1);
    due_payment = block::tlb::t_Grams.as_integer_skip(cs2);
    if (due_payment.is_null() || !cs2.empty_ext()) {
      return false;
    }
  } else {
    due_payment = td::zero_refint();
  }
  unsigned long long u = 0;
  u |= storage_used.cells = block::tlb::t_VarUInteger_7.as_uint(*used.cells);
  u |= storage_used.bits = block::tlb::t_VarUInteger_7.as_uint(*used.bits);
  LOG(DEBUG) << "last_paid=" << last_paid << "; cells=" << storage_used.cells << " bits=" << storage_used.bits;
  return (u != std::numeric_limits<td::uint64>::max());
}

/**
 * Unpacks the state of an Account from a CellSlice.
 *
 * State is serialized using StateInit TLB-scheme.
 * Initializes fixed_prefix_length (from account state - StateInit)
 *
 * @param cs The CellSlice containing the serialized state.
 *
 * @returns True if the state was successfully unpacked, False otherwise.
 */
bool Account::unpack_state(vm::CellSlice& cs) {
  block::gen::StateInit::Record state;
  if (!tlb::unpack_exact(cs, state)) {
    return false;
  }
  fixed_prefix_length = 0;
  if (state.fixed_prefix_length->size() == 6) {
    fixed_prefix_length = (int)state.fixed_prefix_length->prefetch_ulong(6) - 32;
  }
  if (state.special->size() > 1) {
    int z = (int)state.special->prefetch_ulong(3);
    if (z < 0) {
      return false;
    }
    tick = z & 2;
    tock = z & 1;
    LOG(DEBUG) << "tick=" << tick << ", tock=" << tock;
  }
  code = state.code->prefetch_ref();
  data = state.data->prefetch_ref();
  library = orig_library = state.library->prefetch_ref();
  return true;
}

/**
 * Computes the address of the account.
 *
 * @param force If set to true, the address will be recomputed even if it already exists.
 *
 * @returns True if the address was successfully computed, false otherwise.
 */
bool Account::compute_my_addr(bool force) {
  if (!force && my_addr.not_null() && my_addr_exact.not_null()) {
    return true;
  }
  if (workchain == ton::workchainInvalid) {
    my_addr.clear();
    return false;
  }
  vm::CellBuilder cb;
  Ref<vm::Cell> cell, cell2;
  if (workchain >= -128 && workchain < 127) {
    if (!(cb.store_long_bool(2, 2)                             // addr_std$10
          && store_maybe_anycast(cb)                           // anycast:(Maybe Anycast)
          && cb.store_long_rchk_bool(workchain, 8)             // workchain_id:int8
          && cb.store_bits_bool(addr_orig)                     // addr:bits256
          && cb.finalize_to(cell) && cb.store_long_bool(4, 3)  // addr_std$10 anycast:(Maybe Anycast)
          && cb.store_long_rchk_bool(workchain, 8)             // workchain_id:int8
          && cb.store_bits_bool(addr)                          // addr:bits256
          && cb.finalize_to(cell2))) {
      return false;
    }
  } else {
    if (!(cb.store_long_bool(3, 2)                             // addr_var$11
          && store_maybe_anycast(cb)                           // anycast:(Maybe Anycast)
          && cb.store_long_bool(256, 9)                        // addr_len:(## 9)
          && cb.store_long_rchk_bool(workchain, 32)            // workchain_id:int32
          && cb.store_bits_bool(addr_orig)                     // addr:(bits addr_len)
          && cb.finalize_to(cell) && cb.store_long_bool(6, 3)  // addr_var$11 anycast:(Maybe Anycast)
          && cb.store_long_bool(256, 9)                        // addr_len:(## 9)
          && cb.store_long_rchk_bool(workchain, 32)            // workchain_id:int32
          && cb.store_bits_bool(addr)                          // addr:(bits addr_len)
          && cb.finalize_to(cell2))) {
      return false;
    }
  }
  my_addr = load_cell_slice_ref(std::move(cell));
  my_addr_exact = load_cell_slice_ref(std::move(cell2));
  return true;
}

/**
 * Computes the address of the Account.
 *
 * Legacy (used only if global_version < 10).
 *
 * @param tmp_addr A reference to the CellSlice for the result.
 * @param fixed_prefix_length The fixed prefix length for the address.
 * @param orig_addr_rewrite Address prefix of length fixed_prefix_length.
 *
 * @returns True if the address was successfully computed, false otherwise.
 */
bool Account::recompute_tmp_addr(Ref<vm::CellSlice>& tmp_addr, int fixed_prefix_length,
                                 td::ConstBitPtr orig_addr_rewrite) const {
  if (!fixed_prefix_length && my_addr_exact.not_null()) {
    tmp_addr = my_addr_exact;
    return true;
  }
  if (fixed_prefix_length == addr_rewrite_length && my_addr.not_null()) {
    tmp_addr = my_addr;
    return true;
  }
  if (fixed_prefix_length < 0 || fixed_prefix_length > 30) {
    return false;
  }
  vm::CellBuilder cb;
  bool std = (workchain >= -128 && workchain < 128);
  if (!cb.store_long_bool(std ? 2 : 3, 2)) {  // addr_std$10 or addr_var$11
    return false;
  }
  if (!fixed_prefix_length) {
    if (!cb.store_bool_bool(false)) {  // anycast:(Maybe Anycast)
      return false;
    }
  } else if (!(cb.store_bool_bool(true)                             // just$1
               && cb.store_long_bool(fixed_prefix_length, 5)                // depth:(#<= 30)
               && cb.store_bits_bool(addr.bits(), fixed_prefix_length))) {  // rewrite_pfx:(bits depth)
    return false;
  }
  if (std) {
    if (!cb.store_long_rchk_bool(workchain, 8)) {  // workchain:int8
      return false;
    }
  } else if (!(cb.store_long_bool(256, 9)                // addr_len:(## 9)
               && cb.store_long_bool(workchain, 32))) {  // workchain:int32
    return false;
  }
  Ref<vm::Cell> cell;
  return cb.store_bits_bool(orig_addr_rewrite, fixed_prefix_length)  // address:(bits addr_len) or bits256
         && cb.store_bits_bool(addr.bits() + fixed_prefix_length, 256 - fixed_prefix_length) && cb.finalize_to(cell) &&
         (tmp_addr = vm::load_cell_slice_ref(std::move(cell))).not_null();
}

/**
 * Sets address rewriting info for a newly-activated account.
 *
 * @param rewrite_length The fixed prefix length for the account address.
 * @param orig_addr_rewrite Address prefix of length fixed_prefix_length.
 *
 * @returns True if the rewriting info was successfully set, false otherwise.
 */
bool Account::init_rewrite_addr(int rewrite_length, td::ConstBitPtr orig_addr_rewrite) {
  if (addr_rewrite_length_set || !set_addr_rewrite_length(rewrite_length)) {
    return false;
  }
  addr_orig = addr;
  addr_rewrite = addr.bits();
  addr_orig.bits().copy_from(orig_addr_rewrite, rewrite_length);
  return compute_my_addr(true);
}

/**
 * Unpacks the account information from the provided CellSlice.
 * 
 * Used to unpack previously existing accounts.
 *
 * @param shard_account The ShardAccount to unpack.
 * @param now The current Unix time.
 * @param special Flag indicating if the account is special.
 *
 * @returns True if the unpacking is successful, false otherwise.
 */
bool Account::unpack(Ref<vm::CellSlice> shard_account, ton::UnixTime now, bool special) {
  LOG(DEBUG) << "unpacking " << (special ? "special " : "") << "account " << addr.to_hex();
  if (shard_account.is_null()) {
    LOG(ERROR) << "account " << addr.to_hex() << " does not have a valid ShardAccount to unpack";
    return false;
  }
  if (verbosity > 2) {
    FLOG(INFO) {
      shard_account->print_rec(sb, 2);
      block::gen::t_ShardAccount.print(sb, shard_account);
    };
  }
  block::gen::ShardAccount::Record acc_info;
  if (!(block::tlb::t_ShardAccount.validate_csr(shard_account) && tlb::unpack_exact(shard_account.write(), acc_info))) {
    LOG(ERROR) << "account " << addr.to_hex() << " state is invalid";
    return false;
  }
  last_trans_lt_ = acc_info.last_trans_lt;
  last_trans_hash_ = acc_info.last_trans_hash;
  now_ = now;
  auto account = std::move(acc_info.account);
  total_state = orig_total_state = account;
  auto acc_cs = load_cell_slice(std::move(account));
  if (block::gen::t_Account.get_tag(acc_cs) == block::gen::Account::account_none) {
    is_special = special;
    return acc_cs.size_ext() == 1 && init_new(now);
  }
  block::gen::Account::Record_account acc;
  block::gen::AccountStorage::Record storage;
  if (!(tlb::unpack_exact(acc_cs, acc) && (my_addr = acc.addr).not_null() && unpack_address(acc.addr.write()) &&
        compute_my_addr() && unpack_storage_info(acc.storage_stat.write()) &&
        tlb::csr_unpack(this->storage = std::move(acc.storage), storage) &&
        std::max(storage.last_trans_lt, 1ULL) > acc_info.last_trans_lt && balance.unpack(std::move(storage.balance)))) {
    return false;
  }
  is_special = special;
  last_trans_end_lt_ = storage.last_trans_lt;
  switch (block::gen::t_AccountState.get_tag(*storage.state)) {
    case block::gen::AccountState::account_uninit:
      status = orig_status = acc_uninit;
      state_hash = addr;
      forget_addr_rewrite_length();
      break;
    case block::gen::AccountState::account_frozen:
      status = orig_status = acc_frozen;
      if (!storage.state->have(2 + 256)) {
        return false;
      }
      state_hash = storage.state->data_bits() + 2;
      break;
    case block::gen::AccountState::account_active:
      status = orig_status = acc_active;
      if (storage.state.write().fetch_ulong(1) != 1) {
        return false;
      }
      inner_state = storage.state;
      if (!unpack_state(storage.state.write())) {
        return false;
      }
      state_hash.clear();
      break;
    default:
      return false;
  }
  LOG(DEBUG) << "end of Account.unpack() for " << workchain << ":" << addr.to_hex()
             << " (balance = " << balance.to_str() << " ; last_trans_lt = " << last_trans_lt_ << ".."
             << last_trans_end_lt_ << ")";
  return true;
}

/**
 * Initializes a new Account object.
 *
 * @param now The current Unix time.
 *
 * @returns True if the initialization is successful, false otherwise.
 */
bool Account::init_new(ton::UnixTime now) {
  // only workchain and addr are initialized at this point
  if (workchain == ton::workchainInvalid) {
    return false;
  }
  addr_orig = addr;
  addr_rewrite = addr.cbits();
  last_trans_lt_ = last_trans_end_lt_ = 0;
  last_trans_hash_.set_zero();
  now_ = now;
  last_paid = 0;
  storage_used = {};
  storage_dict_hash = {};
  due_payment = td::zero_refint();
  balance.set_zero();
  if (my_addr_exact.is_null()) {
    vm::CellBuilder cb;
    if (workchain >= -128 && workchain < 128) {
      CHECK(cb.store_long_bool(4, 3)                  // addr_std$10 anycast:(Maybe Anycast)
            && cb.store_long_rchk_bool(workchain, 8)  // workchain:int8
            && cb.store_bits_bool(addr));             // address:bits256
    } else {
      CHECK(cb.store_long_bool(0xd00, 12)              // addr_var$11 anycast:(Maybe Anycast) addr_len:(## 9)
            && cb.store_long_rchk_bool(workchain, 32)  // workchain:int32
            && cb.store_bits_bool(addr));              // address:(bits addr_len)
    }
    my_addr_exact = load_cell_slice_ref(cb.finalize());
  }
  if (my_addr.is_null()) {
    my_addr = my_addr_exact;
  }
  if (total_state.is_null()) {
    vm::CellBuilder cb;
    CHECK(cb.store_long_bool(0, 1)  // account_none$0 = Account
          && cb.finalize_to(total_state));
    orig_total_state = total_state;
  }
  state_hash = addr_orig;
  status = orig_status = acc_nonexist;
  addr_rewrite_length_set = false;
  return true;
}

/**
 * Resets the fixed prefix length of the account.
 *
 * @returns True if the fixed prefix length was successfully reset, false otherwise.
 */
bool Account::forget_addr_rewrite_length() {
  addr_rewrite_length_set = false;
  addr_rewrite_length = 0;
  addr_orig = addr;
  my_addr = my_addr_exact;
  addr_rewrite = addr.bits();
  return true;
}

/**
 * Deactivates the account.
 *
 * @returns True if the account was successfully deactivated, false otherwise.
 */
bool Account::deactivate() {
  if (status == acc_active) {
    return false;
  }
  // forget special (tick/tock) info
  tick = tock = false;
  fixed_prefix_length = 0;
  if (status == acc_nonexist || status == acc_uninit) {
    // forget fixed prefix length and address rewriting info
    forget_addr_rewrite_length();
    // forget specific state hash for deleted or uninitialized accounts (revert to addr)
    state_hash = addr;
  }
  // forget code and data (only active accounts remember these)
  code.clear();
  data.clear();
  library.clear();
  // if deleted, balance must be zero
  if (status == acc_nonexist && !balance.is_zero()) {
    return false;
  }
  return true;
}

/**
 * Checks if the account belongs to a specific shard.
 *
 * @param shard The shard to check against.
 *
 * @returns True if the account belongs to the shard, False otherwise.
 */
bool Account::belongs_to_shard(ton::ShardIdFull shard) const {
  return workchain == shard.workchain && ton::shard_is_ancestor(shard.shard, addr);
}

/**
 * Adds the partial storage payment to the total sum.
 *
 * @param payment The total sum to be updated.
 * @param delta The time delta for which the payment is calculated.
 * @param prices The storage prices.
 * @param storage_used Account storage statistics.
 * @param is_mc A flag indicating whether the account is in the masterchain.
 */
void add_partial_storage_payment(td::BigInt256& payment, ton::UnixTime delta, const block::StoragePrices& prices,
                                 const StorageUsed& storage_used, bool is_mc) {
  td::BigInt256 c{(long long)storage_used.cells}, b{(long long)storage_used.bits};
  if (is_mc) {
    // storage.cells * prices.mc_cell_price + storage.bits * prices.mc_bit_price;
    c.mul_short(prices.mc_cell_price);
    b.mul_short(prices.mc_bit_price);
  } else {
    // storage.cells * prices.cell_price + storage.bits * prices.bit_price;
    c.mul_short(prices.cell_price);
    b.mul_short(prices.bit_price);
  }
  b += c;
  b.mul_short(delta).normalize();
  CHECK(b.sgn() >= 0);
  payment += b;
}

/**
 * Computes the storage fees based on the given parameters.
 *
 * @param now The current Unix time.
 * @param pricing The vector of storage prices.
 * @param storage_used Account storage statistics.
 * @param last_paid The Unix time when the last payment was made.
 * @param is_special A flag indicating if the account is special.
 * @param is_masterchain A flag indicating if the account is in the masterchain.
 *
 * @returns The computed storage fees as RefInt256.
 */
td::RefInt256 StoragePrices::compute_storage_fees(ton::UnixTime now, const std::vector<block::StoragePrices>& pricing,
                                                  const StorageUsed& storage_used, ton::UnixTime last_paid,
                                                  bool is_special, bool is_masterchain) {
  if (now <= last_paid || !last_paid || is_special || pricing.empty() || now <= pricing[0].valid_since) {
    return td::zero_refint();
  }
  std::size_t n = pricing.size(), i = n;
  while (i && pricing[i - 1].valid_since > last_paid) {
    --i;
  }
  if (i) {
    --i;
  }
  ton::UnixTime upto = std::max(last_paid, pricing[0].valid_since);
  td::RefInt256 total{true, 0};
  for (; i < n && upto < now; i++) {
    ton::UnixTime valid_until = (i < n - 1 ? std::min(now, pricing[i + 1].valid_since) : now);
    if (upto < valid_until) {
      assert(upto >= pricing[i].valid_since);
      add_partial_storage_payment(total.unique_write(), valid_until - upto, pricing[i], storage_used, is_masterchain);
    }
    upto = valid_until;
  }
  return td::rshift(total, 16, 1);  // divide by 2^16 with ceil rounding to obtain nanograms
}

/**
 * Computes the storage fees for the account.
 *
 * @param now The current Unix time.
 * @param pricing The vector of storage prices.
 *
 * @returns The computed storage fees as RefInt256.
 */
td::RefInt256 Account::compute_storage_fees(ton::UnixTime now, const std::vector<block::StoragePrices>& pricing) const {
  return StoragePrices::compute_storage_fees(now, pricing, storage_used, last_paid, is_special, is_masterchain());
}

namespace transaction {
/**
 * Constructs a new Transaction object.
 *
 * @param _account The Account object.
 * @param ttype The type of the transaction (see transaction.cpp#309).
 * @param req_start_lt The minimal logical time of the transaction.
 * @param _now The current Unix time.
 * @param _inmsg The input message that caused the transaction.
 *
 * @returns None
 */
Transaction::Transaction(const Account& _account, int ttype, ton::LogicalTime req_start_lt, ton::UnixTime _now,
                         Ref<vm::Cell> _inmsg)
    : trans_type(ttype)
    , is_first(_account.transactions.empty())
    , new_tick(_account.tick)
    , new_tock(_account.tock)
    , new_fixed_prefix_length(_account.fixed_prefix_length)
    , now(_now)
    , account(_account)
    , my_addr(_account.my_addr)
    , my_addr_exact(_account.my_addr_exact)
    , balance(_account.balance)
    , original_balance(_account.balance)
    , due_payment(_account.due_payment)
    , last_paid(_account.last_paid)
    , new_code(_account.code)
    , new_data(_account.data)
    , new_library(_account.library)
    , in_msg(std::move(_inmsg)) {
  start_lt = std::max(req_start_lt, account.last_trans_end_lt_);
  end_lt = start_lt + 1;
  acc_status = (account.status == Account::acc_nonexist ? Account::acc_uninit : account.status);
  if (acc_status == Account::acc_frozen) {
    frozen_hash = account.state_hash;
  }
}

/**
 * Unpacks the input message of a transaction.
 *
 * @param ihr_delivered A boolean indicating whether the message was delivered using IHR (Instant Hypercube Routing).
 * @param cfg Action phase configuration.
 *
 * @returns A boolean indicating whether the unpacking was successful.
 */
bool Transaction::unpack_input_msg(bool ihr_delivered, const ActionPhaseConfig* cfg) {
  if (in_msg.is_null() || in_msg_type) {
    return false;
  }
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "unpacking inbound message for a new transaction: ";
      block::gen::t_Message_Any.print_ref(sb, in_msg);
      load_cell_slice(in_msg).print_rec(sb);
    };
  }
  auto cs = vm::load_cell_slice(in_msg);
  int tag = gen::t_CommonMsgInfo.get_tag(cs);
  switch (tag) {
    case gen::CommonMsgInfo::int_msg_info: {
      if (!(tlb::unpack(cs, in_msg_info) && msg_balance_remaining.unpack(in_msg_info.value))) {
        return false;
      }
      if (in_msg_info.ihr_disabled && ihr_delivered) {
        return false;
      }
      bounce_enabled = in_msg_info.bounce;
      in_msg_type = 1;
      td::RefInt256 ihr_fee = block::tlb::t_Grams.as_integer(in_msg_info.ihr_fee);
      if (ihr_delivered) {
        in_fwd_fee = std::move(ihr_fee);
      } else {
        in_fwd_fee = td::zero_refint();
        msg_balance_remaining += std::move(ihr_fee);
      }
      if (in_msg_info.created_lt >= start_lt) {
        start_lt = in_msg_info.created_lt + 1;
        end_lt = start_lt + 1;
      }
      // ...
      break;
    }
    case gen::CommonMsgInfo::ext_in_msg_info: {
      gen::CommonMsgInfo::Record_ext_in_msg_info info;
      if (!tlb::unpack(cs, info)) {
        return false;
      }
      in_msg_info.ihr_disabled = in_msg_info.bounce = in_msg_info.bounced = false;
      in_msg_info.src = info.src;
      in_msg_info.dest = info.dest;
      in_msg_info.created_at = in_msg_info.created_lt = 0;
      if (cfg->disable_anycast) {
        // Check that dest is addr_std without anycast
        gen::MsgAddressInt::Record_addr_std rec;
        if (!gen::csr_unpack(info.dest, rec)) {
          LOG(DEBUG) << "destination address of the external message is not a valid addr_std";
          return false;
        }
        if (rec.anycast->size() > 1) {
          LOG(DEBUG) << "destination address of the external message is an anycast address";
          return false;
        }
      }
      in_msg_type = 2;
      in_msg_extern = true;
      // compute forwarding fees for this external message
      vm::CellStorageStat sstat;                                     // for message size
      auto cell_info = sstat.compute_used_storage(cs).move_as_ok();  // message body
      sstat.bits -= cs.size();                                       // bits in the root cells are free
      sstat.cells--;                                                 // the root cell itself is not counted as a cell
      LOG(DEBUG) << "storage paid for a message: " << sstat.cells << " cells, " << sstat.bits << " bits";
      if (sstat.bits > cfg->size_limits.max_msg_bits || sstat.cells > cfg->size_limits.max_msg_cells) {
        LOG(DEBUG) << "inbound external message too large, invalid";
        return false;
      }
      if (cell_info.max_merkle_depth > max_allowed_merkle_depth) {
        LOG(DEBUG) << "inbound external message has too big merkle depth, invalid";
        return false;
      }
      // fetch message pricing info
      CHECK(cfg);
      const MsgPrices& msg_prices = cfg->fetch_msg_prices(account.is_masterchain());
      // compute forwarding fees
      auto fees_c = msg_prices.compute_fwd_ihr_fees(sstat.cells, sstat.bits, true);
      LOG(DEBUG) << "computed fwd fees = " << fees_c.first << " + " << fees_c.second;

      if (account.is_special) {
        LOG(DEBUG) << "computed fwd fees set to zero for special account";
        fees_c.first = fees_c.second = 0;
      }
      in_fwd_fee = td::make_refint(fees_c.first);
      if (balance.grams < in_fwd_fee) {
        LOG(DEBUG) << "cannot pay for importing this external message";
        return false;
      }
      // (tentatively) debit account for importing this external message
      balance -= in_fwd_fee;
      msg_balance_remaining.set_zero();  // external messages cannot carry value
      // ...
      break;
    }
    default:
      return false;
  }
  // init:(Maybe (Either StateInit ^StateInit))
  switch ((int)cs.prefetch_ulong(2)) {
    case 2: {  // (just$1 (left$0 _:StateInit ))
      Ref<vm::CellSlice> state_init;
      vm::CellBuilder cb;
      if (!(cs.advance(2) && block::gen::t_StateInit.fetch_to(cs, state_init) &&
            cb.append_cellslice_bool(std::move(state_init)) && cb.finalize_to(in_msg_state) &&
            block::gen::t_StateInitWithLibs.validate_ref(in_msg_state))) {
        LOG(DEBUG) << "cannot parse StateInit in inbound message";
        return false;
      }
      break;
    }
    case 3: {  // (just$1 (right$1 _:^StateInit ))
      if (!(cs.advance(2) && cs.fetch_ref_to(in_msg_state) &&
            block::gen::t_StateInitWithLibs.validate_ref(in_msg_state))) {
        LOG(DEBUG) << "cannot parse ^StateInit in inbound message";
        return false;
      }
      break;
    }
    default:  // nothing$0
      if (!cs.advance(1)) {
        LOG(DEBUG) << "invalid init field in an inbound message";
        return false;
      }
  }
  // body:(Either X ^X)
  switch ((int)cs.fetch_ulong(1)) {
    case 0:  // left$0 _:X
      in_msg_body = Ref<vm::CellSlice>{true, cs};
      break;
    case 1:  // right$1 _:^X
      if (cs.size_ext() != 0x10000) {
        LOG(DEBUG) << "body of an inbound message is not represented by exactly one reference";
        return false;
      }
      in_msg_body = load_cell_slice_ref(cs.prefetch_ref());
      break;
    default:
      LOG(DEBUG) << "invalid body field in an inbound message";
      return false;
  }
  total_fees += in_fwd_fee;
  if (account.workchain == ton::masterchainId && cfg->mc_blackhole_addr &&
      cfg->mc_blackhole_addr.value() == account.addr) {
    blackhole_burned.grams = msg_balance_remaining.grams;
    msg_balance_remaining.grams = td::zero_refint();
    LOG(DEBUG) << "Burning " << blackhole_burned.grams << " nanoton (blackhole address)";
  }
  return true;
}

/**
 * Prepares the storage phase of a transaction.
 *
 * @param cfg The configuration for the storage phase.
 * @param force_collect Flag indicating whether to collect fees for frozen accounts.
 * @param adjust_msg_value Flag indicating whether to adjust the message value if the account balance becomes less than the message balance.
 *
 * @returns True if the storage phase was successfully prepared, false otherwise.
 */
bool Transaction::prepare_storage_phase(const StoragePhaseConfig& cfg, bool force_collect, bool adjust_msg_value) {
  if (now < account.last_paid) {
    return false;
  }
  auto to_pay = account.compute_storage_fees(now, *(cfg.pricing)) + due_payment;
  if (to_pay.not_null() && sgn(to_pay) < 0) {
    return false;
  }
  auto res = std::make_unique<StoragePhase>();
  res->is_special = account.is_special;
  last_paid = res->last_paid_updated = (res->is_special ? 0 : now);
  if (to_pay.is_null() || sgn(to_pay) == 0) {
    res->fees_collected = res->fees_due = td::zero_refint();
  } else if (to_pay <= balance.grams) {
    res->fees_collected = to_pay;
    res->fees_due = td::zero_refint();
    balance -= std::move(to_pay);
    if (cfg.global_version >= 7) {
      due_payment = td::zero_refint();
    }
  } else if (acc_status == Account::acc_frozen && !force_collect && to_pay < cfg.delete_due_limit) {
    // do not collect fee
    res->last_paid_updated = (res->is_special ? 0 : account.last_paid);
    res->fees_collected = res->fees_due = td::zero_refint();
  } else {
    res->fees_collected = balance.grams;
    res->fees_due = std::move(to_pay) - std::move(balance.grams);
    balance.grams = td::zero_refint();
    if (!res->is_special) {
      auto total_due = res->fees_due;
      switch (acc_status) {
        case Account::acc_uninit:
        case Account::acc_frozen:
          if (total_due > cfg.delete_due_limit && balance.extra.is_null()) {
            // Keeping accounts with non-null extras is a temporary measure before implementing proper collection of
            // extracurrencies from deleted accounts
            res->deleted = true;
            acc_status = Account::acc_deleted;
            if (balance.extra.not_null()) {
              // collect extra currencies as a fee
              total_fees += block::CurrencyCollection{0, std::move(balance.extra)};
              balance.extra.clear();
            }
          }
          break;
        case Account::acc_active:
          if (total_due > cfg.freeze_due_limit) {
            res->frozen = true;
            was_frozen = true;
            acc_status = Account::acc_frozen;
          }
          break;
      }
      if (cfg.enable_due_payment) {
        due_payment = total_due;
      }
    }
  }
  if (adjust_msg_value && msg_balance_remaining.grams > balance.grams) {
    msg_balance_remaining.grams = balance.grams;
  }
  total_fees += res->fees_collected;
  storage_phase = std::move(res);
  return true;
}

/**
 * Prepares the credit phase of a transaction.
 *
 * This function creates a CreditPhase object and performs the necessary calculations
 * to determine the amount to be credited in the credit phase. It updates the due payment,
 * credit, balance, and total fees accordingly.
 *
 * @returns True if the credit phase is prepared successfully, false otherwise.
 */
bool Transaction::prepare_credit_phase() {
  credit_phase = std::make_unique<CreditPhase>();
  // Due payment is only collected in storage phase.
  // For messages with bounce flag, contract always receives the amount specified in message
  // auto collected = std::min(msg_balance_remaining.grams, due_payment);
  // credit_phase->due_fees_collected = collected;
  // due_payment -= collected;
  // credit_phase->credit = msg_balance_remaining -= collected;
  credit_phase->due_fees_collected = td::zero_refint();
  credit_phase->credit = msg_balance_remaining;
  if (!msg_balance_remaining.is_valid()) {
    LOG(ERROR) << "cannot compute the amount to be credited in the credit phase of transaction";
    return false;
  }
  // NB: msg_balance_remaining may be deducted from balance later during bounce phase
  balance += msg_balance_remaining;
  if (!balance.is_valid()) {
    LOG(ERROR) << "cannot credit currency collection to account";
    return false;
  }
  // total_fees += std::move(collected);
  return true;
}
}  // namespace transaction

/**
 * Parses the gas limits and prices from a given cell.
 *
 * @param cell The cell containing the gas limits and prices serialized using GasLimitsPricing TLB-scheme.
 * @param freeze_due_limit Reference to store the freeze due limit.
 * @param delete_due_limit Reference to store the delete due limit.
 *
 * @returns True if the parsing is successful, false otherwise.
 */
bool ComputePhaseConfig::parse_GasLimitsPrices(Ref<vm::Cell> cell, td::RefInt256& freeze_due_limit,
                                               td::RefInt256& delete_due_limit) {
  return cell.not_null() &&
         parse_GasLimitsPrices(vm::load_cell_slice_ref(std::move(cell)), freeze_due_limit, delete_due_limit);
}

/**
 * Parses the gas limits and prices from a given cell slice.
 *
 * @param cs The cell slice containing the gas limits and prices serialized using GasLimitsPricing TLB-scheme.
 * @param freeze_due_limit Reference to store the freeze due limit.
 * @param delete_due_limit Reference to store the delete due limit.
 *
 * @returns True if the parsing is successful, false otherwise.
 */
bool ComputePhaseConfig::parse_GasLimitsPrices(Ref<vm::CellSlice> cs, td::RefInt256& freeze_due_limit,
                                               td::RefInt256& delete_due_limit) {
  if (cs.is_null()) {
    return false;
  }
  block::gen::GasLimitsPrices::Record_gas_flat_pfx flat;
  if (tlb::csr_unpack(cs, flat)) {
    return parse_GasLimitsPrices_internal(std::move(flat.other), freeze_due_limit, delete_due_limit,
                                          flat.flat_gas_limit, flat.flat_gas_price);
  } else {
    return parse_GasLimitsPrices_internal(std::move(cs), freeze_due_limit, delete_due_limit);
  }
}

/**
 * Parses the gas limits and prices from a gas limits and prices record.
 *
 * @param cs The cell slice containing the gas limits and prices serialized using GasLimitsPricing TLB-scheme.
 * @param freeze_due_limit A reference to store the freeze due limit.
 * @param delete_due_limit A reference to store the delete due limit.
 * @param _flat_gas_limit The flat gas limit.
 * @param _flat_gas_price The flat gas price.
 *
 * @returns True if the parsing is successful, false otherwise.
 */
bool ComputePhaseConfig::parse_GasLimitsPrices_internal(Ref<vm::CellSlice> cs, td::RefInt256& freeze_due_limit,
                                                        td::RefInt256& delete_due_limit, td::uint64 _flat_gas_limit,
                                                        td::uint64 _flat_gas_price) {
  auto f = [&](const auto& r, td::uint64 spec_limit) {
    gas_limit = r.gas_limit;
    special_gas_limit = spec_limit;
    gas_credit = r.gas_credit;
    gas_price = r.gas_price;
    freeze_due_limit = td::make_refint(r.freeze_due_limit);
    delete_due_limit = td::make_refint(r.delete_due_limit);
  };
  block::gen::GasLimitsPrices::Record_gas_prices_ext rec;
  if (tlb::csr_unpack(cs, rec)) {
    f(rec, rec.special_gas_limit);
  } else {
    block::gen::GasLimitsPrices::Record_gas_prices rec0;
    if (tlb::csr_unpack(std::move(cs), rec0)) {
      f(rec0, rec0.gas_limit);
    } else {
      return false;
    }
  }
  flat_gas_limit = _flat_gas_limit;
  flat_gas_price = _flat_gas_price;
  compute_threshold();
  return true;
}

/**
 * Checks if an address is suspended according to the ConfigParam(44).
 *
 * @param wc The workchain ID.
 * @param addr The account address address.
 *
 * @returns True if the address is suspended, False otherwise.
 */
bool ComputePhaseConfig::is_address_suspended(ton::WorkchainId wc, td::Bits256 addr) const {
  if (!suspended_addresses) {
    return false;
  }
  try {
    vm::CellBuilder key;
    key.store_long_bool(wc, 32);
    key.store_bits_bool(addr);
    return !suspended_addresses->lookup(key.data_bits(), 288).is_null();
  } catch (vm::VmError) {
    return false;
  }
}

/**
 * Computes the maximum gas fee based on the gas prices and limits.
 *
 * @param gas_price256 The gas price from config as RefInt256
 * @param gas_limit The gas limit from config
 * @param flat_gas_limit The flat gas limit from config
 * @param flat_gas_price The flat gas price from config
 *
 * @returns The maximum gas fee.
 */
static td::RefInt256 compute_max_gas_threshold(const td::RefInt256& gas_price256, td::uint64 gas_limit,
                                               td::uint64 flat_gas_limit, td::uint64 flat_gas_price) {
  if (gas_limit > flat_gas_limit) {
    return td::rshift(gas_price256 * (gas_limit - flat_gas_limit), 16, 1) + td::make_bigint(flat_gas_price);
  } else {
    return td::make_refint(flat_gas_price);
  }
}

/**
 * Computes the maximum for gas fee based on the gas prices and limits.
 *
 * Updates max_gas_threshold.
 */
void ComputePhaseConfig::compute_threshold() {
  gas_price256 = td::make_refint(gas_price);
  max_gas_threshold = compute_max_gas_threshold(gas_price256, gas_limit, flat_gas_limit, flat_gas_price);
}

/**
 * Computes the amount of gas that can be bought for a given amount of nanograms.
 *
 * @param nanograms The amount of nanograms to compute gas for.
 *
 * @returns The amount of gas.
 */
td::uint64 ComputePhaseConfig::gas_bought_for(td::RefInt256 nanograms) const {
  if (nanograms.is_null() || sgn(nanograms) < 0) {
    return 0;
  }
  if (nanograms >= max_gas_threshold) {
    return gas_limit;
  }
  if (nanograms < flat_gas_price) {
    return 0;
  }
  auto res = td::div((std::move(nanograms) - flat_gas_price) << 16, gas_price256);
  return res->to_long() + flat_gas_limit;
}

/**
 * Computes the gas price.
 *
 * @param gas_used The amount of gas used.
 *
 * @returns The computed gas price.
 */
td::RefInt256 ComputePhaseConfig::compute_gas_price(td::uint64 gas_used) const {
  return gas_used <= flat_gas_limit ? td::make_refint(flat_gas_price)
                                    : td::rshift(gas_price256 * (gas_used - flat_gas_limit), 16, 1) + flat_gas_price;
}

namespace transaction {

/**
 * Checks if it is required to increase gas_limit (from GasLimitsPrices config) for the transaction
 *
 * In January 2024 a highload wallet of @wallet Telegram bot in mainnet was stuck because current gas limit (1M) is
 * not enough to clean up old queires, thus locking funds inside.
 * See comment in crypto/smartcont/highload-wallet-v2-code.fc for details on why this happened.
 * Account address: EQD_v9j1rlsuHHw2FIhcsCFFSD367ldfDdCKcsNmNpIRzUlu
 * It was proposed to validators to increase gas limit for this account to 70M for a limited amount
 * of time (until 2024-02-29).
 * It is activated by setting global version to 5 in ConfigParam 8.
 * This config change also activates new behavior for special accounts in masterchain.
 *
 * In August 2024 it was decided to unlock other old highload wallets that got into the same situation.
 * See https://t.me/tondev_news/129
 * It is activated by setting global version to 9.
 *
 * @param cfg The compute phase configuration.
 * @param now The Unix time of the transaction.
 * @param account The account of the transaction.
 *
 * @returns Overridden gas limit or empty td::optional
 */
static td::optional<td::uint64> override_gas_limit(const ComputePhaseConfig& cfg, ton::UnixTime now,
                                                   const Account& account) {
  struct OverridenGasLimit {
    td::uint64 new_limit;
    int from_version;
    ton::UnixTime until;
  };
  static std::map<std::pair<ton::WorkchainId, ton::StdSmcAddress>, OverridenGasLimit> accounts = []() {
    auto parse_addr = [](const char* s) -> std::pair<ton::WorkchainId, ton::StdSmcAddress> {
      auto r_addr = StdAddress::parse(td::Slice(s));
      r_addr.ensure();
      return {r_addr.ok().workchain, r_addr.ok().addr};
    };
    std::map<std::pair<ton::WorkchainId, ton::StdSmcAddress>, OverridenGasLimit> accounts;

    // Increase limit for EQD_v9j1rlsuHHw2FIhcsCFFSD367ldfDdCKcsNmNpIRzUlu until 2024-02-29 00:00:00 UTC
    accounts[parse_addr("0:FFBFD8F5AE5B2E1C7C3614885CB02145483DFAEE575F0DD08A72C366369211CD")] = {
        .new_limit = 70'000'000, .from_version = 5, .until = 1709164800};

    // Increase limit for multiple accounts (https://t.me/tondev_news/129) until 2025-03-01 00:00:00 UTC
    accounts[parse_addr("UQBeSl-dumOHieZ3DJkNKVkjeso7wZ0VpzR4LCbLGTQ8xr57")] = {
        .new_limit = 70'000'000, .from_version = 9, .until = 1740787200};
    accounts[parse_addr("EQC3VcQ-43klww9UfimR58TBjBzk7GPupXQ3CNuthoNp-uTR")] = {
        .new_limit = 70'000'000, .from_version = 9, .until = 1740787200};
    accounts[parse_addr("EQBhwBb8jvokGvfreHRRoeVxI237PrOJgyrsAhLA-4rBC_H5")] = {
        .new_limit = 70'000'000, .from_version = 9, .until = 1740787200};
    accounts[parse_addr("EQCkoRp4OE-SFUoMEnYfL3vF43T3AzNfW8jyTC4yzk8cJqMS")] = {
        .new_limit = 70'000'000, .from_version = 9, .until = 1740787200};
    accounts[parse_addr("UQBN5ICras79U8FYEm71ws34n-ZNIQ0LRNpckOUsIV3OebnC")] = {
        .new_limit = 70'000'000, .from_version = 9, .until = 1740787200};
    accounts[parse_addr("EQBDanbCeUqI4_v-xrnAN0_I2wRvEIaLg1Qg2ZN5c6Zl1KOh")] = {
        .new_limit = 225'000'000, .from_version = 9, .until = 1740787200};
    return accounts;
  }();
  auto it = accounts.find({account.workchain, account.addr});
  if (it == accounts.end() || cfg.global_version < it->second.from_version || now >= it->second.until) {
    return {};
  }
  return it->second.new_limit;
}

/**
 * Computes the amount of gas that can be bought for a given amount of nanograms.
 * Usually equal to `cfg.gas_bought_for(nanograms)`
 * However, it overrides gas_limit from config in special cases.
 *
 * @param cfg The compute phase configuration.
 * @param nanograms The amount of nanograms to compute gas for.
 *
 * @returns The amount of gas.
 */
td::uint64 Transaction::gas_bought_for(const ComputePhaseConfig& cfg, td::RefInt256 nanograms) {
  if (auto new_limit = override_gas_limit(cfg, now, account)) {
    gas_limit_overridden = true;
    // Same as ComputePhaseConfig::gas_bought for, but with other gas_limit and max_gas_threshold
    auto gas_limit = new_limit.value();
    LOG(INFO) << "overridding gas limit for account " << account.workchain << ":" << account.addr.to_hex() << " to "
              << gas_limit;
    auto max_gas_threshold =
        compute_max_gas_threshold(cfg.gas_price256, gas_limit, cfg.flat_gas_limit, cfg.flat_gas_price);
    if (nanograms.is_null() || sgn(nanograms) < 0) {
      return 0;
    }
    if (nanograms >= max_gas_threshold) {
      return gas_limit;
    }
    if (nanograms < cfg.flat_gas_price) {
      return 0;
    }
    auto res = td::div((std::move(nanograms) - cfg.flat_gas_price) << 16, cfg.gas_price256);
    return res->to_long() + cfg.flat_gas_limit;
  }
  return cfg.gas_bought_for(nanograms);
}

/**
 * Computes the gas limits for a transaction.
 *
 * @param cp The ComputePhase object to store the computed gas limits.
 * @param cfg The compute phase configuration.
 *
 * @returns True if the gas limits were successfully computed, false otherwise.
 */
bool Transaction::compute_gas_limits(ComputePhase& cp, const ComputePhaseConfig& cfg) {
  // Compute gas limits
  if (account.is_special) {
    cp.gas_max = cfg.special_gas_limit;
  } else {
    cp.gas_max = gas_bought_for(cfg, balance.grams);
  }
  if (trans_type != tr_ord || (account.is_special && cfg.special_gas_full)) {
    // may use all gas that can be bought using remaining balance
    cp.gas_limit = cp.gas_max;
  } else {
    // originally use only gas bought using remaining message balance
    // if the message is "accepted" by the smart contract, the gas limit will be set to gas_max
    cp.gas_limit = std::min(gas_bought_for(cfg, msg_balance_remaining.grams), cp.gas_max);
  }
  if (trans_type == tr_ord && !block::tlb::t_Message.is_internal(in_msg)) {
    // external messages carry no balance, give them some credit to check whether they are accepted
    cp.gas_credit = std::min(cfg.gas_credit, cp.gas_max);
  } else {
    cp.gas_credit = 0;
  }
  LOG(DEBUG) << "gas limits: max=" << cp.gas_max << ", limit=" << cp.gas_limit << ", credit=" << cp.gas_credit;
  return true;
}

/**
 * Prepares a TVM stack for a transaction.
 *
 * @param cp The compute phase object.
 *
 * @returns A reference to the prepared virtual machine stack.
 *          Returns an empty reference if the transaction type is invalid.
 */
Ref<vm::Stack> Transaction::prepare_vm_stack(ComputePhase& cp) {
  Ref<vm::Stack> stack_ref{true};
  td::RefInt256 acc_addr{true};
  CHECK(acc_addr.write().import_bits(account.addr.cbits(), 256));
  vm::Stack& stack = stack_ref.write();
  switch (trans_type) {
    case tr_tick:
    case tr_tock:
      stack.push_int(balance.grams);
      stack.push_int(std::move(acc_addr));
      stack.push_bool(trans_type == tr_tock);
      stack.push_smallint(-2);
      return stack_ref;
    case tr_ord:
      stack.push_int(balance.grams);
      stack.push_int(msg_balance_remaining.grams);
      stack.push_cell(in_msg);
      stack.push_cellslice(in_msg_body);
      stack.push_bool(in_msg_extern);
      return stack_ref;
    default:
      LOG(ERROR) << "cannot initialize stack for a transaction of type " << trans_type;
      return {};
  }
}

/**
 * Prepares a random seed for a transaction.
 *
 * @param rand_seed The output random seed.
 * @param cfg The configuration for the compute phase.
 *
 * @returns True if the random seed was successfully prepared, false otherwise.
 */
bool Transaction::prepare_rand_seed(td::BitArray<256>& rand_seed, const ComputePhaseConfig& cfg) const {
  // we might use SHA256(block_rand_seed . addr . trans_lt)
  // instead, we use SHA256(block_rand_seed . addr)
  // if the smart contract wants to randomize further, it can use RANDOMIZE instruction
  td::BitArray<256 + 256> data;
  data.bits().copy_from(cfg.block_rand_seed.cbits(), 256);
  if (cfg.global_version >= 8) {
    (data.bits() + 256).copy_from(account.addr.cbits(), 256);
  } else {
    (data.bits() + 256).copy_from(account.addr_rewrite.cbits(), 256);
  }
  rand_seed.clear();
  data.compute_sha256(rand_seed);
  return true;
}

/**
 * Prepares the c7 tuple (virtual machine context) for a compute phase of a transaction.
 *
 * @param cfg The configuration for the compute phase.
 *
 * @returns A reference to a Tuple object.
 *
 * @throws CollatorError if the rand_seed cannot be computed for the transaction.
 */
Ref<vm::Tuple> Transaction::prepare_vm_c7(const ComputePhaseConfig& cfg) const {
  td::BitArray<256> rand_seed;
  td::RefInt256 rand_seed_int{true};
  if (!(prepare_rand_seed(rand_seed, cfg) && rand_seed_int.unique_write().import_bits(rand_seed.cbits(), 256, false))) {
    LOG(ERROR) << "cannot compute rand_seed for transaction";
    throw CollatorError{"cannot generate valid SmartContractInfo"};
    return {};
  }
  std::vector<vm::StackEntry> tuple = {
      td::make_refint(0x076ef1ea),                // [ magic:0x076ef1ea
      td::zero_refint(),                          //   actions:Integer
      td::zero_refint(),                          //   msgs_sent:Integer
      td::make_refint(now),                       //   unixtime:Integer
      td::make_refint(account.block_lt),          //   block_lt:Integer
      td::make_refint(start_lt),                  //   trans_lt:Integer
      std::move(rand_seed_int),                   //   rand_seed:Integer
      balance.as_vm_tuple(),                      //   balance_remaining:[Integer (Maybe Cell)]
      my_addr,                                    //   myself:MsgAddressInt
      vm::StackEntry::maybe(cfg.global_config)    //   global_config:(Maybe Cell) ] = SmartContractInfo;
  };
  if (cfg.global_version >= 4) {
    tuple.push_back(vm::StackEntry::maybe(new_code));  // code:Cell
    if (msg_balance_remaining.is_valid()) {
      tuple.push_back(msg_balance_remaining.as_vm_tuple());  // in_msg_value:[Integer (Maybe Cell)]
    } else {
      tuple.push_back(block::CurrencyCollection::zero().as_vm_tuple());
    }
    tuple.push_back(storage_phase->fees_collected);       // storage_fees:Integer

    // See crypto/block/mc-config.cpp#2223 (get_prev_blocks_info)
    // [ wc:Integer shard:Integer seqno:Integer root_hash:Integer file_hash:Integer] = BlockId;
    // [ last_mc_blocks:[BlockId...]
    //   prev_key_block:BlockId
    //   last_mc_blocks_100:[BlockId...] ] : PrevBlocksInfo
    // The only context where PrevBlocksInfo (13 parameter of c7) is null is inside emulator
    // where it need to be set via transaction_emulator_set_prev_blocks_info (see emulator/emulator-extern.cpp)
    // Inside validator, collator and liteserver checking external message  contexts
    // prev_blocks_info is always not null, since get_prev_blocks_info()  
    // may only return tuple or raise Error (See crypto/block/mc-config.cpp#2223)
    tuple.push_back(vm::StackEntry::maybe(cfg.prev_blocks_info));
  }
  if (cfg.global_version >= 6) {
    tuple.push_back(vm::StackEntry::maybe(cfg.unpacked_config_tuple));          // unpacked_config_tuple:[...]
    tuple.push_back(due_payment.not_null() ? due_payment : td::zero_refint());  // due_payment:Integer
    tuple.push_back(compute_phase->precompiled_gas_usage
                        ? vm::StackEntry(td::make_refint(compute_phase->precompiled_gas_usage.value()))
                        : vm::StackEntry());  // precompiled_gas_usage:Integer
  }
  if (cfg.global_version >= 11) {
    // in_msg_params:[...]
    tuple.push_back(prepare_in_msg_params_tuple(trans_type == tr_ord ? &in_msg_info : nullptr, in_msg_state,
                                                msg_balance_remaining));
  }
  auto tuple_ref = td::make_cnt_ref<std::vector<vm::StackEntry>>(std::move(tuple));
  LOG(DEBUG) << "SmartContractInfo initialized with " << vm::StackEntry(tuple_ref).to_string();
  return vm::make_tuple_ref(std::move(tuple_ref));
}

/**
 * Prepares tuple with unpacked parameters of the inbound message (for the 17th element of c7).
 * `info` is:
 * - For internal messages - just int_msg_info of the message
 * - For external messages - artificial int_msg_info based on ext_msg_info of the messages.
 * - For tick-tock transactions and get methods - nullptr.
 *
 * @param info Pointer to the message info.
 * @param state_init State init of the message (null if absent).
 * @param msg_balance_remaining Remaining balance of the message (it's sometimes different from value in info).
 *
 * @returns Tuple with message parameters.
 */
Ref<vm::Tuple> Transaction::prepare_in_msg_params_tuple(const gen::CommonMsgInfo::Record_int_msg_info* info,
                                                        const Ref<vm::Cell>& state_init,
                                                        const CurrencyCollection& msg_balance_remaining) {
  std::vector<vm::StackEntry> in_msg_params(10);
  if (info != nullptr) {
    in_msg_params[0] = td::make_refint(info->bounce ? -1 : 0);   // bounce
    in_msg_params[1] = td::make_refint(info->bounced ? -1 : 0);  // bounced
    in_msg_params[2] = info->src;                                // src_addr
    in_msg_params[3] = info->fwd_fee.is_null() ? td::zero_refint() : tlb::t_Grams.as_integer(info->fwd_fee);  // fwd_fee
    in_msg_params[4] = td::make_refint(info->created_lt);  // created_lt
    in_msg_params[5] = td::make_refint(info->created_at);  // created_at
    auto value = info->value;
    in_msg_params[6] =
        info->value.is_null() ? td::zero_refint() : tlb::t_Grams.as_integer_skip(value.write());  // original value
    in_msg_params[7] = msg_balance_remaining.is_valid() ? msg_balance_remaining.grams : td::zero_refint();  // value
    in_msg_params[8] = msg_balance_remaining.is_valid() ? vm::StackEntry::maybe(msg_balance_remaining.extra)
                                                        : vm::StackEntry{};  // value extra
    in_msg_params[9] = vm::StackEntry::maybe(state_init);                    // state_init
  } else {
    in_msg_params[0] = td::zero_refint();  // bounce
    in_msg_params[1] = td::zero_refint();  // bounced
    static Ref<vm::CellSlice> addr_none = vm::CellBuilder{}.store_zeroes(2).as_cellslice_ref();
    in_msg_params[2] = addr_none;          // src_addr
    in_msg_params[3] = td::zero_refint();  // fed_fee
    in_msg_params[4] = td::zero_refint();  // created_lt
    in_msg_params[5] = td::zero_refint();  // created_at
    in_msg_params[6] = td::zero_refint();  // original value
    in_msg_params[7] = td::zero_refint();  // value
    in_msg_params[8] = vm::StackEntry{};   // value extra
    in_msg_params[9] = vm::StackEntry{};   // state_init
  }
  return td::make_cnt_ref<std::vector<vm::StackEntry>>(std::move(in_msg_params));
}

/**
 * Computes the number of output actions in a list.
 *
 * @param list c5 cell.
 *
 * @returns The number of output actions.
 */
int output_actions_count(Ref<vm::Cell> list) {
  int i = -1;
  do {
    ++i;
    bool special = true;
    auto cs = load_cell_slice_special(std::move(list), special);
    if (special) {
      break;
    }
    list = cs.prefetch_ref();
  } while (list.not_null());
  return i;
}

/**
 * Unpacks the message StateInit.
 *
 * @param cfg The configuration for the compute phase.
 * @param lib_only If true, only unpack libraries from the state.
 * @param forbid_public_libs Don't allow public libraries in initstate.
 *
 * @returns True if the unpacking is successful, false otherwise.
 */
bool Transaction::unpack_msg_state(const ComputePhaseConfig& cfg, bool lib_only, bool forbid_public_libs) {
  block::gen::StateInit::Record state;
  if (in_msg_state.is_null() || !tlb::unpack_cell(in_msg_state, state)) {
    LOG(ERROR) << "cannot unpack StateInit from an inbound message";
    return false;
  }
  if (lib_only) {
    in_msg_library = state.library->prefetch_ref();
    return true;
  }
  if (state.fixed_prefix_length->size() == 6) {
    new_fixed_prefix_length = (signed char)(state.fixed_prefix_length->prefetch_ulong(6) - 32);
  } else {
    new_fixed_prefix_length = 0;
  }
  if (!cfg.disable_anycast) {
    new_addr_rewrite_length = new_fixed_prefix_length;
  }
  if (state.special->size() > 1) {
    int z = (int)state.special->prefetch_ulong(3);
    if (z < 0) {
      return false;
    }
    new_tick = z & 2;
    new_tock = z & 1;
    LOG(DEBUG) << "tick=" << new_tick << ", tock=" << new_tock;
  }
  td::Ref<vm::Cell> old_code = new_code, old_data = new_data, old_library = new_library;
  new_code = state.code->prefetch_ref();
  new_data = state.data->prefetch_ref();
  new_library = state.library->prefetch_ref();
  auto size_limits = cfg.size_limits;
  if (forbid_public_libs) {
    size_limits.max_acc_public_libraries = 0;
  }
  auto S = check_state_limits(size_limits, false);
  if (S.is_error()) {
    LOG(DEBUG) << "Cannot unpack msg state: " << S.move_as_error();
    new_code = old_code;
    new_data = old_data;
    new_library = old_library;
    return false;
  }
  return true;
}

/**
 * Computes the set of libraries to be used during TVM execution.
 *
 * @param cfg The configuration for the compute phase.
 *
 * @returns A vector of hashmaps with libraries.
 */
std::vector<Ref<vm::Cell>> Transaction::compute_vm_libraries(const ComputePhaseConfig& cfg) {
  std::vector<Ref<vm::Cell>> lib_set;
  if (in_msg_library.not_null()) {
    lib_set.push_back(in_msg_library);
  }
  if (new_library.not_null()) {
    lib_set.push_back(new_library);
  }
  auto global_libs = cfg.get_lib_root();
  if (global_libs.not_null()) {
    lib_set.push_back(std::move(global_libs));
  }
  return lib_set;
}

/**
 * Checks if the input message StateInit hash corresponds to the account address.
 *
 * @param cfg The configuration for the compute phase.
 *
 * @returns True if the input message state hash is valid, False otherwise.
 */
bool Transaction::check_in_msg_state_hash(const ComputePhaseConfig& cfg) {
  CHECK(in_msg_state.not_null());
  CHECK(new_fixed_prefix_length >= 0 && new_fixed_prefix_length < 32);
  td::Bits256 in_state_hash = in_msg_state->get_hash().bits();
  int d = new_fixed_prefix_length;
  if ((in_state_hash.bits() + d).compare(account.addr.bits() + d, 256 - d)) {
    return false;
  }
  orig_addr_rewrite = in_state_hash.bits();
  orig_addr_rewrite_set = true;
  if (cfg.disable_anycast) {
    my_addr = my_addr_exact;
    return true;
  } else {
    return account.recompute_tmp_addr(my_addr, d, orig_addr_rewrite.bits());
  }
}

/**
 * Runs the precompiled smart contract and prepares the compute phase.
 *
 * @param cfg The configuration for the compute phase.
 * @param impl Implementation of the smart contract
 *
 * @returns True if the contract was successfully executed, false otherwise.
 */
bool Transaction::run_precompiled_contract(const ComputePhaseConfig& cfg, precompiled::PrecompiledSmartContract& impl) {
  ComputePhase& cp = *compute_phase;
  CHECK(cp.precompiled_gas_usage);
  td::uint64 gas_usage = cp.precompiled_gas_usage.value();
  td::Timer timer;
  auto result =
      impl.run(my_addr, now, start_lt, balance, new_data, *in_msg_body, in_msg, msg_balance_remaining, in_msg_extern,
               compute_vm_libraries(cfg), cfg.global_version, cfg.max_vm_data_depth, new_code,
               cfg.unpacked_config_tuple, due_payment.not_null() ? due_payment : td::zero_refint(), gas_usage);
  double elapsed = timer.elapsed();
  cp.vm_init_state_hash = td::Bits256::zero();
  cp.exit_code = result.exit_code;
  cp.out_of_gas = false;
  cp.vm_final_state_hash = td::Bits256::zero();
  cp.vm_steps = 0;
  cp.gas_used = gas_usage;
  cp.accepted = result.accepted;
  cp.success = (cp.accepted && result.committed);
  LOG(INFO) << "Running precompiled smart contract " << impl.get_name() << ": exit_code=" << result.exit_code
            << " accepted=" << result.accepted << " success=" << cp.success << " gas_used=" << gas_usage
            << " time=" << elapsed << "s";
  if (cp.accepted & use_msg_state) {
    was_activated = true;
    acc_status = Account::acc_active;
  }
  if (cfg.with_vm_log) {
    cp.vm_log = PSTRING() << "Running precompiled smart contract " << impl.get_name()
                          << ": exit_code=" << result.exit_code << " accepted=" << result.accepted
                          << " success=" << cp.success << " gas_used=" << gas_usage << " time=" << elapsed << "s";
  }
  if (cp.success) {
    cp.new_data = impl.get_c4();
    cp.actions = impl.get_c5();
    int out_act_num = output_actions_count(cp.actions);
    if (verbosity > 2) {
      FLOG(INFO) {
        sb << "new smart contract data: ";
        bool can_be_special = true;
        load_cell_slice_special(cp.new_data, can_be_special).print_rec(sb);
        sb << "output actions: ";
        block::gen::OutList{out_act_num}.print_ref(sb, cp.actions);
      };
    }
  }
  cp.mode = 0;
  cp.exit_arg = 0;
  if (!cp.success && result.exit_arg) {
    auto value = td::narrow_cast_safe<td::int32>(result.exit_arg.value());
    if (value.is_ok()) {
      cp.exit_arg = value.ok();
    }
  }
  if (cp.accepted) {
    if (account.is_special) {
      cp.gas_fees = td::zero_refint();
    } else {
      cp.gas_fees = cfg.compute_gas_price(cp.gas_used);
      total_fees += cp.gas_fees;
      balance -= cp.gas_fees;
    }
    LOG(DEBUG) << "gas fees: " << cp.gas_fees->to_dec_string() << " = " << cfg.gas_price256->to_dec_string() << " * "
               << cp.gas_used << " /2^16 ; price=" << cfg.gas_price << "; flat rate=[" << cfg.flat_gas_price << " for "
               << cfg.flat_gas_limit << "]; remaining balance=" << balance.to_str();
    CHECK(td::sgn(balance.grams) >= 0);
  }
  return true;
}

/**
 * Prepares the compute phase of a transaction, which includes running TVM.
 *
 * @param cfg The configuration for the compute phase.
 *
 * @returns True if the compute phase was successfully prepared and executed, false otherwise.
 */
bool Transaction::prepare_compute_phase(const ComputePhaseConfig& cfg) {
  // TODO: add more skip verifications + sometimes use state from in_msg to re-activate
  // ...
  compute_phase = std::make_unique<ComputePhase>();
  ComputePhase& cp = *(compute_phase.get());
  if (cfg.global_version >= 9) {
    original_balance = balance;
    if (msg_balance_remaining.is_valid()) {
      original_balance -= msg_balance_remaining;
    }
  } else {
    original_balance -= total_fees;
  }
  if (td::sgn(balance.grams) <= 0) {
    // no gas
    cp.skip_reason = ComputePhase::sk_no_gas;
    return true;
  }
  // Compute gas limits
  if (!compute_gas_limits(cp, cfg)) {
    compute_phase.reset();
    return false;
  }
  if (!cp.gas_limit && !cp.gas_credit) {
    // no gas
    cp.skip_reason = ComputePhase::sk_no_gas;
    return true;
  }
  if (in_msg_state.not_null()) {
    LOG(DEBUG) << "HASH(in_msg_state) = " << in_msg_state->get_hash().bits().to_hex(256)
               << ", account_state_hash = " << account.state_hash.to_hex();
  } else {
    LOG(DEBUG) << "in_msg_state is null";
  }
  if (in_msg_state.not_null() &&
      (acc_status == Account::acc_uninit ||
       (acc_status == Account::acc_frozen && account.state_hash == in_msg_state->get_hash().bits()))) {
    if (acc_status == Account::acc_uninit && cfg.is_address_suspended(account.workchain, account.addr)) {
      LOG(DEBUG) << "address is suspended, skipping compute phase";
      cp.skip_reason = ComputePhase::sk_suspended;
      return true;
    }
    use_msg_state = true;
    bool forbid_public_libs =
        acc_status == Account::acc_uninit && account.is_masterchain();  // Forbid for deploying, allow for unfreezing
    if (!(unpack_msg_state(cfg, false, forbid_public_libs) &&
          account.check_addr_rewrite_length(new_fixed_prefix_length))) {
      LOG(DEBUG) << "cannot unpack in_msg_state, or it has bad fixed_prefix_length; cannot init account state";
      cp.skip_reason = ComputePhase::sk_bad_state;
      return true;
    }
    if (acc_status == Account::acc_uninit && !check_in_msg_state_hash(cfg)) {
      LOG(DEBUG) << "in_msg_state hash mismatch, cannot init account state";
      cp.skip_reason = ComputePhase::sk_bad_state;
      return true;
    }
    if (cfg.disable_anycast && acc_status == Account::acc_uninit &&
        new_fixed_prefix_length > cfg.size_limits.max_acc_fixed_prefix_length) {
      LOG(DEBUG) << "cannot init account state: too big fixed prefix length (" << new_fixed_prefix_length << ", max "
                 << cfg.size_limits.max_acc_fixed_prefix_length << ")";
      cp.skip_reason = ComputePhase::sk_bad_state;
      return true;
    }
  } else if (acc_status != Account::acc_active) {
    // no state, cannot perform transactions
    cp.skip_reason = in_msg_state.not_null() ? ComputePhase::sk_bad_state : ComputePhase::sk_no_state;
    return true;
  } else if (in_msg_state.not_null()) {
    if (cfg.allow_external_unfreeze) {
      if (in_msg_extern && account.addr != in_msg_state->get_hash().bits()) {
        // only for external messages with non-zero initstate in active accounts
        LOG(DEBUG) << "in_msg_state hash mismatch in external message";
        cp.skip_reason = ComputePhase::sk_bad_state;
        return true;
      }
    }
    unpack_msg_state(cfg, true);  // use only libraries
  }
  if (!cfg.allow_external_unfreeze) {
    if (in_msg_extern && in_msg_state.not_null() && account.addr != in_msg_state->get_hash().bits()) {
      LOG(DEBUG) << "in_msg_state hash mismatch in external message";
      cp.skip_reason = ComputePhase::sk_bad_state;
      return true;
    }
  }
  if (cfg.disable_anycast) {
    my_addr = my_addr_exact;
    new_addr_rewrite_length = 0;
    force_remove_anycast_address = true;
  }

  td::optional<PrecompiledContractsConfig::Contract> precompiled;
  if (new_code.not_null() && trans_type == tr_ord) {
    precompiled = cfg.precompiled_contracts.get_contract(new_code->get_hash().bits());
  }

  vm::GasLimits gas{(long long)cp.gas_limit, (long long)cp.gas_max, (long long)cp.gas_credit};
  if (precompiled) {
    td::uint64 gas_usage = precompiled.value().gas_usage;
    cp.precompiled_gas_usage = gas_usage;
    if (gas_usage > cp.gas_limit) {
      cp.skip_reason = ComputePhase::sk_no_gas;
      return true;
    }
    auto impl = precompiled::get_implementation(new_code->get_hash().bits());
    if (impl != nullptr && !cfg.dont_run_precompiled_ && impl->required_version() <= cfg.global_version) {
      return run_precompiled_contract(cfg, *impl);
    }

    // Contract is marked as precompiled in global config, but implementation is not available
    // In this case we run TVM and override gas_used
    LOG(INFO) << "Unknown precompiled contract (code_hash=" << new_code->get_hash().to_hex()
              << ", gas_usage=" << gas_usage << "), running VM";
    long long limit = account.is_special ? cfg.special_gas_limit : cfg.gas_limit;
    gas = vm::GasLimits{limit, limit, gas.gas_credit ? limit : 0};
  }

  // initialize VM
  Ref<vm::Stack> stack = prepare_vm_stack(cp);
  if (stack.is_null()) {
    compute_phase.reset();
    return false;
  }
  // OstreamLogger ostream_logger(error_stream);
  // auto log = create_vm_log(error_stream ? &ostream_logger : nullptr);
  LOG(DEBUG) << "creating VM";

  std::unique_ptr<StringLoggerTail> logger;
  auto vm_log = vm::VmLog();
  if (cfg.with_vm_log) {
    size_t log_max_size = 256;
    if (cfg.vm_log_verbosity > 4) {
      log_max_size = 32 << 20;
    } else if (cfg.vm_log_verbosity > 0) {
      log_max_size = 1 << 20;
    }
    logger = std::make_unique<StringLoggerTail>(log_max_size);
    vm_log.log_interface = logger.get();
    vm_log.log_options = td::LogOptions(VERBOSITY_NAME(DEBUG), true, false);
    if (cfg.vm_log_verbosity > 1) {
      vm_log.log_mask |= vm::VmLog::ExecLocation;
      if (cfg.vm_log_verbosity > 2) {
        vm_log.log_mask |= vm::VmLog::GasRemaining;
        if (cfg.vm_log_verbosity > 3) {
          vm_log.log_mask |= vm::VmLog::DumpStack;
          if (cfg.vm_log_verbosity > 4) {
            vm_log.log_mask |= vm::VmLog::DumpStackVerbose;
            vm_log.log_mask |= vm::VmLog::DumpC5;
          }
        }
      }
    }
  }
  vm::VmState vm{new_code, cfg.global_version, std::move(stack), gas, 1, new_data, vm_log, compute_vm_libraries(cfg)};
  vm.set_max_data_depth(cfg.max_vm_data_depth);
  vm.set_c7(prepare_vm_c7(cfg));  // tuple with SmartContractInfo
  vm.set_chksig_always_succeed(cfg.ignore_chksig);
  vm.set_stop_on_accept_message(cfg.stop_on_accept_message);
  // vm.incr_stack_trace(1);    // enable stack dump after each step

  LOG(DEBUG) << "starting VM";
  cp.vm_init_state_hash = vm.get_state_hash();
  td::Timer timer;
  cp.exit_code = ~vm.run();
  double elapsed = timer.elapsed();
  LOG(DEBUG) << "VM terminated with exit code " << cp.exit_code;
  cp.out_of_gas = (cp.exit_code == ~(int)vm::Excno::out_of_gas);
  cp.vm_final_state_hash = vm.get_final_state_hash(cp.exit_code);
  stack = vm.get_stack_ref();
  cp.vm_steps = (int)vm.get_steps_count();
  gas = vm.get_gas_limits();
  cp.gas_used = std::min<long long>(gas.gas_consumed(), gas.gas_limit);
  cp.accepted = (gas.gas_credit == 0);
  cp.success = (cp.accepted && vm.committed());
  if (cp.accepted & use_msg_state) {
    was_activated = true;
    acc_status = Account::acc_active;
  }
  if (precompiled) {
    cp.gas_used = precompiled.value().gas_usage;
    cp.vm_steps = 0;
    cp.vm_init_state_hash = cp.vm_final_state_hash = td::Bits256::zero();
    if (cp.out_of_gas) {
      LOG(ERROR) << "Precompiled smc got out_of_gas in TVM";
      return false;
    }
  }
  LOG(INFO) << "steps: " << vm.get_steps_count() << " gas: used=" << gas.gas_consumed() << ", max=" << gas.gas_max
            << ", limit=" << gas.gas_limit << ", credit=" << gas.gas_credit;
  LOG(INFO) << "out_of_gas=" << cp.out_of_gas << ", accepted=" << cp.accepted << ", success=" << cp.success
            << ", time=" << elapsed << "s";
  if (logger != nullptr) {
    cp.vm_log = logger->get_log();
  }
  if (cp.success) {
    cp.new_data = vm.get_committed_state().c4;  // c4 -> persistent data
    cp.actions = vm.get_committed_state().c5;   // c5 -> action list
    int out_act_num = output_actions_count(cp.actions);
    if (verbosity > 2) {
      FLOG(INFO) {
        sb << "new smart contract data: ";
        bool can_be_special = true;
        load_cell_slice_special(cp.new_data, can_be_special).print_rec(sb);
        sb << "output actions: ";
        block::gen::OutList{out_act_num}.print_ref(sb, cp.actions);
      };
    }
  }
  cp.mode = 0;
  cp.exit_arg = 0;
  if (!cp.success && stack->depth() > 0) {
    td::RefInt256 tos = stack->tos().as_int();
    if (tos.not_null() && tos->signed_fits_bits(32)) {
      cp.exit_arg = (int)tos->to_long();
    }
  }
  if (cp.accepted) {
    if (account.is_special) {
      cp.gas_fees = td::zero_refint();
    } else {
      cp.gas_fees = cfg.compute_gas_price(cp.gas_used);
      total_fees += cp.gas_fees;
      balance -= cp.gas_fees;
    }
    LOG(DEBUG) << "gas fees: " << cp.gas_fees->to_dec_string() << " = " << cfg.gas_price256->to_dec_string() << " * "
               << cp.gas_used << " /2^16 ; price=" << cfg.gas_price << "; flat rate=[" << cfg.flat_gas_price << " for "
               << cfg.flat_gas_limit << "]; remaining balance=" << balance.to_str();
    CHECK(td::sgn(balance.grams) >= 0);
  }
  return true;
}

/**
 * Prepares the action phase of a transaction.
 *
 * @param cfg The configuration for the action phase.
 *
 * @returns True if the action phase was prepared successfully, false otherwise.
 */
bool Transaction::prepare_action_phase(const ActionPhaseConfig& cfg) {
  if (!compute_phase || !compute_phase->success) {
    return false;
  }
  action_phase = std::make_unique<ActionPhase>();
  ActionPhase& ap = *(action_phase.get());
  ap.result_code = -1;
  ap.result_arg = 0;
  ap.tot_actions = ap.spec_actions = ap.skipped_actions = ap.msgs_created = 0;
  Ref<vm::Cell> list = compute_phase->actions;
  assert(list.not_null());
  ap.action_list_hash = list->get_hash().bits();
  ap.remaining_balance = balance;
  ap.end_lt = end_lt;
  ap.total_fwd_fees = td::zero_refint();
  ap.total_action_fees = td::zero_refint();
  ap.reserved_balance.set_zero();
  ap.action_fine = td::zero_refint();

  td::Ref<vm::Cell> old_code = new_code, old_data = new_data, old_library = new_library;
  auto enforce_state_limits = [&]() {
    if (account.is_special) {
      return true;
    }
    auto S = check_state_limits(cfg.size_limits);
    if (S.is_error()) {
      // Rollback changes to state, fail action phase
      LOG(INFO) << "Account state size exceeded limits: " << S.move_as_error();
      new_account_storage_stat = {};
      new_code = old_code;
      new_data = old_data;
      new_library = old_library;
      ap.result_code = 50;
      ap.state_exceeds_limits = true;
      return false;
    }
    return true;
  };

  int n = 0;
  while (true) {
    ap.action_list.push_back(list);
    bool special = true;
    auto cs = load_cell_slice_special(std::move(list), special);
    if (special) {
      ap.result_code = 32;  // action list invalid
      ap.result_arg = n;
      ap.action_list_invalid = true;
      LOG(DEBUG) << "action list invalid: special cell";
      return true;
    }
    if (!cs.size_ext()) {
      break;
    }
    if (!cs.have_refs()) {
      ap.result_code = 32;  // action list invalid
      ap.result_arg = n;
      ap.action_list_invalid = true;
      LOG(DEBUG) << "action list invalid: entry found with data but no next reference";
      return true;
    }
    list = cs.prefetch_ref();
    n++;
    if (n > cfg.max_actions) {
      ap.result_code = 33;  // too many actions
      ap.result_arg = n;
      ap.action_list_invalid = true;
      LOG(DEBUG) << "action list too long: more than " << cfg.max_actions << " actions";
      return true;
    }
  }

  ap.tot_actions = n;
  ap.spec_actions = ap.skipped_actions = 0;
  for (int i = n - 1; i >= 0; --i) {
    ap.result_arg = n - 1 - i;
    if (!block::gen::t_OutListNode.validate_ref(ap.action_list[i])) {
      if (cfg.message_skip_enabled) {
        // try to read mode from action_send_msg even if out_msg scheme is violated
        // action should at least contain 40 bits: 32bit tag and 8 bit mode
        // if (mode & 2), that is ignore error mode, skip action even for invalid message
        // if there is no (mode & 2) but (mode & 16) presents - enable bounce if possible
        bool special = true;
        auto cs = load_cell_slice_special(ap.action_list[i], special);
        if (!special) {
          if ((cs.size() >= 40) && ((int)cs.fetch_ulong(32) == 0x0ec3c86d)) {
            int mode = (int)cs.fetch_ulong(8);
            if (mode & 2) {
              ap.skipped_actions++;
              ap.action_list[i] = {};
              continue;
            } else if ((mode & 16) && cfg.bounce_on_fail_enabled) {
              ap.bounce = true;
            }
          }
        }
      }
      ap.result_code = 34;  // action #i invalid or unsupported
      ap.action_list_invalid = true;
      LOG(DEBUG) << "invalid action " << ap.result_arg << " found while preprocessing action list: error code "
                 << ap.result_code;
      return true;
    }
  }
  ap.valid = true;
  for (int i = n - 1; i >= 0; --i) {
    if(ap.action_list[i].is_null()) {
      continue;
    }
    ap.result_arg = n - 1 - i;
    vm::CellSlice cs = load_cell_slice(ap.action_list[i]);
    CHECK(cs.fetch_ref().not_null());
    int tag = block::gen::t_OutAction.get_tag(cs);
    CHECK(tag >= 0);
    int err_code = 34;
    ap.need_bounce_on_fail = false;
    switch (tag) {
      case block::gen::OutAction::action_set_code:
        err_code = try_action_set_code(cs, ap, cfg);
        break;
      case block::gen::OutAction::action_send_msg:
        err_code = try_action_send_msg(cs, ap, cfg);
        if (err_code == -2) {
          err_code = try_action_send_msg(cs, ap, cfg, 1);
          if (err_code == -2) {
            err_code = try_action_send_msg(cs, ap, cfg, 2);
          }
        }
        break;
      case block::gen::OutAction::action_reserve_currency:
        err_code = try_action_reserve_currency(cs, ap, cfg);
        break;
      case block::gen::OutAction::action_change_library:
        err_code = try_action_change_library(cs, ap, cfg);
        break;
    }
    if (err_code) {
      ap.result_code = (err_code == -1 ? 34 : err_code);
      ap.end_lt = end_lt;
      if (err_code == -1 || err_code == 34) {
        ap.action_list_invalid = true;
      }
      if (err_code == 37 || err_code == 38) {
        ap.no_funds = true;
      }
      LOG(DEBUG) << "invalid action " << ap.result_arg << " in action list: error code " << ap.result_code;
      // This is required here because changes to libraries are applied even if action phase fails
      enforce_state_limits();
      if (cfg.action_fine_enabled) {
        ap.action_fine = std::min(ap.action_fine, balance.grams);
        ap.total_action_fees = ap.action_fine;
        balance.grams -= ap.action_fine;
        total_fees += ap.action_fine;
      }
      if (ap.need_bounce_on_fail) {
        ap.bounce = true;
      }
      return true;
    }
  }

  if (cfg.action_fine_enabled) {
    ap.total_action_fees += ap.action_fine;
  }
  end_lt = ap.end_lt;
  if (ap.new_code.not_null()) {
    new_code = ap.new_code;
  }
  new_data = compute_phase->new_data;  // tentative persistent data update applied
  if (!enforce_state_limits()) {
    if (cfg.extra_currency_v2) {
      end_lt = ap.end_lt = start_lt + 1;
      if (cfg.action_fine_enabled) {
        ap.action_fine = std::min(ap.action_fine, balance.grams);
        ap.total_action_fees = ap.action_fine;
        balance.grams -= ap.action_fine;
        total_fees += ap.action_fine;
      }
    }
    return true;
  }

  ap.result_arg = 0;
  ap.result_code = 0;
  CHECK(ap.remaining_balance.grams->sgn() >= 0);
  CHECK(ap.reserved_balance.grams->sgn() >= 0);
  ap.remaining_balance += ap.reserved_balance;
  CHECK(ap.remaining_balance.is_valid());
  if (ap.acc_delete_req) {
    CHECK(cfg.extra_currency_v2 ? ap.remaining_balance.grams->sgn() == 0 : ap.remaining_balance.is_zero());
    ap.acc_status_change = ActionPhase::acst_deleted;
    acc_status = (ap.remaining_balance.is_zero() ? Account::acc_deleted : Account::acc_uninit);
    was_deleted = true;
  }
  ap.success = true;
  out_msgs = std::move(ap.out_msgs);
  total_fees +=
      ap.total_action_fees;  // NB: forwarding fees are not accounted here (they are not collected by the validators in this transaction)
  balance = ap.remaining_balance;
  return true;
}

/**
 * Tries to set the code for an account.
 *
 * @param cs The CellSlice containing the action data serialized as action_set_code TLB-scheme.
 * @param ap The action phase object.
 * @param cfg The action phase configuration.
 *
 * @returns 0 if the code was successfully set, -1 otherwise.
 */
int Transaction::try_action_set_code(vm::CellSlice& cs, ActionPhase& ap, const ActionPhaseConfig& cfg) {
  block::gen::OutAction::Record_action_set_code rec;
  if (!tlb::unpack_exact(cs, rec)) {
    return -1;
  }
  ap.new_code = std::move(rec.new_code);
  ap.code_changed = true;
  ap.spec_actions++;
  return 0;
}

/**
 * Tries to change the library in the transaction.
 *
 * @param cs The cell slice containing the action data serialized as action_change_library TLB-scheme.
 * @param ap The action phase object.
 * @param cfg The action phase configuration.
 *
 * @returns 0 if the action was successfully performed,
 *          -1 if there was an error unpacking the data or the mode is invalid,
 *          41 if the library reference is required but is null,
 *          43 if the number of cells in the library exceeds the limit,
 *          42 if there was a VM error during the operation.
 */
int Transaction::try_action_change_library(vm::CellSlice& cs, ActionPhase& ap, const ActionPhaseConfig& cfg) {
  block::gen::OutAction::Record_action_change_library rec;
  if (!tlb::unpack_exact(cs, rec)) {
    return -1;
  }
  // mode: +0 = remove library, +1 = add private library, +2 = add public library, +16 - bounce on fail
  if (rec.mode & 16) {
    if (!cfg.bounce_on_fail_enabled) {
      return -1;
    }
    ap.need_bounce_on_fail = true;
    rec.mode &= ~16;
  }
  if (rec.mode > 2) {
    return -1;
  }
  Ref<vm::Cell> lib_ref = rec.libref->prefetch_ref();
  ton::Bits256 hash;
  if (lib_ref.not_null()) {
    hash = lib_ref->get_hash().bits();
  } else {
    CHECK(rec.libref.write().fetch_ulong(1) == 0 && rec.libref.write().fetch_bits_to(hash));
  }
  try {
    vm::Dictionary dict{new_library, 256};
    if (!rec.mode) {
      // remove library
      dict.lookup_delete(hash);
      LOG(DEBUG) << "removed " << ((rec.mode >> 1) ? "public" : "private") << " library with hash " << hash.to_hex();
    } else {
      auto val = dict.lookup(hash);
      if (val.not_null()) {
        bool is_public = val->prefetch_ulong(1);
        auto ref = val->prefetch_ref();
        if (hash == ref->get_hash().bits()) {
          lib_ref = ref;
          if (is_public == (rec.mode >> 1)) {
            // library already in required state
            ap.spec_actions++;
            return 0;
          }
        }
      }
      if (lib_ref.is_null()) {
        // library code not found
        return 41;
      }
      vm::CellStorageStat sstat;
      auto cell_info = sstat.compute_used_storage(lib_ref).move_as_ok();
      if (sstat.cells > cfg.size_limits.max_library_cells || cell_info.max_merkle_depth > max_allowed_merkle_depth) {
        return 43;
      }
      vm::CellBuilder cb;
      CHECK(cb.store_bool_bool(rec.mode >> 1) && cb.store_ref_bool(std::move(lib_ref)));
      CHECK(dict.set_builder(hash, cb));
      LOG(DEBUG) << "added " << ((rec.mode >> 1) ? "public" : "private") << " library with hash " << hash.to_hex();
    }
    new_library = std::move(dict).extract_root_cell();
  } catch (vm::VmError&) {
    return 42;
  }
  ap.spec_actions++;
  return 0;
}
}  // namespace transaction

/**
 * Computes the forward fees for a message based on the number of cells and bits.
 * 
 * msg_fwd_fees = (lump_price + ceil((bit_price * msg.bits + cell_price * msg.cells)/2^16)) nanograms
 * ihr_fwd_fees = ceil((msg_fwd_fees * ihr_price_factor)/2^16) nanograms
 * bits in the root cell of a message are not included in msg.bits (lump_price pays for them)
 *
 * @param cells The number of cells in the message.
 * @param bits The number of bits in the message.
 *
 * @returns The computed forward fees for the message.
 */
td::uint64 MsgPrices::compute_fwd_fees(td::uint64 cells, td::uint64 bits) const {
  return lump_price + td::uint128(bit_price)
                          .mult(bits)
                          .add(td::uint128(cell_price).mult(cells))
                          .add(td::uint128(0xffffu))
                          .shr(16)
                          .lo();
}

/**
 * Computes the forward fees for a message based on the number of cells and bits.
 * Return the result as td::RefInt256
 *
 * msg_fwd_fees = (lump_price + ceil((bit_price * msg.bits + cell_price * msg.cells)/2^16)) nanograms
 * ihr_fwd_fees = ceil((msg_fwd_fees * ihr_price_factor)/2^16) nanograms
 * bits in the root cell of a message are not included in msg.bits (lump_price pays for them)
 *
 * @param cells The number of cells in the message.
 * @param bits The number of bits in the message.
 *
 * @returns The computed forward fees for the message as td::RefInt256j.
 */
td::RefInt256 MsgPrices::compute_fwd_fees256(td::uint64 cells, td::uint64 bits) const {
  return td::make_refint(lump_price) +
         td::rshift(td::make_refint(bit_price) * bits + td::make_refint(cell_price) * cells, 16,
                    1);  // divide by 2^16 with ceil rounding
}

/**
 * Computes the forward fees and IHR fees for a message with the given number of cells and bits.
 *
 * @param cells The number of cells.
 * @param bits The number of bits.
 * @param ihr_disabled Flag indicating whether IHR is disabled.
 *
 * @returns A pair of values representing the forward fees and IHR fees.
 */
std::pair<td::uint64, td::uint64> MsgPrices::compute_fwd_ihr_fees(td::uint64 cells, td::uint64 bits,
                                                                  bool ihr_disabled) const {
  td::uint64 fwd = compute_fwd_fees(cells, bits);
  if (ihr_disabled) {
    return std::pair<td::uint64, td::uint64>(fwd, 0);
  }
  return std::pair<td::uint64, td::uint64>(fwd, td::uint128(fwd).mult(ihr_factor).shr(16).lo());
}

/**
 * Computes the part of the fees that go to the total fees of the current block.
 *
 * @param total The amount of fees.
 *
 * @returns The the part of the fees that go to the total fees of the current block.
 */
td::RefInt256 MsgPrices::get_first_part(td::RefInt256 total) const {
  return (std::move(total) * first_frac) >> 16;
}

/**
 * Computes the part of the fees that go to the total fees of the current block.
 *
 * @param total The amount of fees.
 *
 * @returns The the part of the fees that go to the total fees of the current block.
 */
td::uint64 MsgPrices::get_first_part(td::uint64 total) const {
  return td::uint128(total).mult(first_frac).shr(16).lo();
}

/**
 * Computes the part of the fees that go to the total fees of the transit block.
 *
 * @param total The amount of fees.
 *
 * @returns The the part of the fees that go to the total fees of the transit block.
 */
td::RefInt256 MsgPrices::get_next_part(td::RefInt256 total) const {
  return (std::move(total) * next_frac) >> 16;
}

namespace transaction {
/**
 * Checks if the source address is addr_none and replaces is with the account address.
 *
 * @param src_addr A reference to the source address of the message.
 *
 * @returns True if the source address is addr_none or is equal to the account address.
 */
bool Transaction::check_replace_src_addr(Ref<vm::CellSlice>& src_addr) const {
  int t = (int)src_addr->prefetch_ulong(2);
  if (!t && src_addr->size_ext() == 2) {
    // addr_none$00  --> replace with the address of current smart contract
    src_addr = my_addr;
    return true;
  }
  if (t != 2) {
    // invalid address (addr_extern and addr_var cannot be source addresses)
    return false;
  }
  if (src_addr->contents_equal(*my_addr) || src_addr->contents_equal(*my_addr_exact)) {
    // source address matches that of the current account
    return true;
  }
  // only one valid case remaining: rewritten source address used, replace with the complete one
  // (are we sure we want to allow this?)
  return false;
}

/**
 * Checks the destination address of a message, rewrites it if it is an anycast address.
 *
 * @param dest_addr A reference to the destination address of the transaction.
 * @param cfg The configuration for the action phase.
 * @param is_mc A pointer to a boolean where it will be stored whether the destination is in the masterchain.
 * @param allow_anycast Allow anycast the address.
 *
 * @returns True if the destination address is valid, false otherwise.
 */
bool Transaction::check_rewrite_dest_addr(Ref<vm::CellSlice>& dest_addr, const ActionPhaseConfig& cfg,
                                          bool* is_mc, bool allow_anycast) const {
  if (!dest_addr->prefetch_ulong(1)) {
    // all external addresses allowed
    if (is_mc) {
      *is_mc = false;
    }
    return true;
  }
  bool repack = false;
  int tag = block::gen::t_MsgAddressInt.get_tag(*dest_addr);

  block::gen::MsgAddressInt::Record_addr_var rec;

  if (tag == block::gen::MsgAddressInt::addr_var) {
    if (!tlb::csr_unpack(dest_addr, rec)) {
      // cannot unpack addr_var
      LOG(DEBUG) << "cannot unpack addr_var in a destination address";
      return false;
    }
    if (rec.addr_len == 256 && rec.workchain_id >= -128 && rec.workchain_id < 128) {
      LOG(DEBUG) << "destination address contains an addr_var to be repacked into addr_std";
      repack = true;
    }
  } else if (tag == block::gen::MsgAddressInt::addr_std) {
    block::gen::MsgAddressInt::Record_addr_std recs;
    if (!tlb::csr_unpack(dest_addr, recs)) {
      // cannot unpack addr_std
      LOG(DEBUG) << "cannot unpack addr_std in a destination address";
      return false;
    }
    rec.anycast = std::move(recs.anycast);
    rec.addr_len = 256;
    rec.workchain_id = recs.workchain_id;
    rec.address = td::make_bitstring_ref(recs.address);
  } else {
    // unknown address format (not a MsgAddressInt)
    LOG(DEBUG) << "destination address does not have a MsgAddressInt tag";
    return false;
  }
  if (rec.workchain_id != ton::masterchainId) {
    // recover destination workchain info from configuration
    auto it = cfg.workchains->find(rec.workchain_id);
    if (it == cfg.workchains->end()) {
      // undefined destination workchain
      LOG(DEBUG) << "destination address contains unknown workchain_id " << rec.workchain_id;
      return false;
    }
    if (!it->second->accept_msgs) {
      // workchain does not accept new messages
      LOG(DEBUG) << "destination address belongs to workchain " << rec.workchain_id << " not accepting new messages";
      return false;
    }
    if (!it->second->is_valid_addr_len(rec.addr_len)) {
      // invalid address length for specified workchain
      LOG(DEBUG) << "destination address has length " << rec.addr_len << " invalid for destination workchain "
                 << rec.workchain_id;
      return false;
    }
  }
  if (rec.anycast->size() > 1) {
    if (!allow_anycast) {
      return false;
    }
    // destination address is an anycast
    vm::CellSlice cs{*rec.anycast};
    int d = (int)cs.fetch_ulong(6) - 32;
    if (d <= 0 || d > 30) {
      // invalid anycast prefix length
      return false;
    }
    unsigned pfx = (unsigned)cs.fetch_ulong(d);
    unsigned my_pfx = (unsigned)account.addr.cbits().get_uint(d);
    if (pfx != my_pfx) {
      // rewrite destination address
      vm::CellBuilder cb;
      CHECK(cb.store_long_bool(32 + d, 6)     // just$1 depth:(#<= 30)
            && cb.store_long_bool(my_pfx, d)  // rewrite_pfx:(bits depth)
            && (rec.anycast = load_cell_slice_ref(cb.finalize())).not_null());
      repack = true;
    }
  }
  if (is_mc) {
    *is_mc = (rec.workchain_id == ton::masterchainId);
  }
  if (!repack) {
    return true;
  }
  if (rec.addr_len == 256 && rec.workchain_id >= -128 && rec.workchain_id < 128) {
    // repack as an addr_std
    vm::CellBuilder cb;
    CHECK(cb.store_long_bool(2, 2)                             // addr_std$10
          && cb.append_cellslice_bool(std::move(rec.anycast))  // anycast:(Maybe Anycast) ...
          && cb.store_long_bool(rec.workchain_id, 8)           // workchain_id:int8
          && cb.append_bitstring(std::move(rec.address))       // address:bits256
          && (dest_addr = load_cell_slice_ref(cb.finalize())).not_null());
  } else {
    // repack as an addr_var
    CHECK(tlb::csr_pack(dest_addr, std::move(rec)));
  }
  CHECK(block::gen::t_MsgAddressInt.validate_csr(dest_addr));
  return true;
}

/**
 * Tries to send a message.
 *
 * @param cs0 The cell slice containing the action data serialized as action_send_msg TLB-scheme.
 * @param ap The action phase.
 * @param cfg The action phase configuration.
 * @param redoing The index of the attempt, starting from 0. On later attempts tries to move message body and StateInit to separate cells.
 *
 * @returns 0 if the message is successfully sent or if the error may be ignored, error code otherwise.
 *          Returns -2 if the action should be attempted again.
 */
int Transaction::try_action_send_msg(const vm::CellSlice& cs0, ActionPhase& ap, const ActionPhaseConfig& cfg,
                                     int redoing) {
  block::gen::OutAction::Record_action_send_msg act_rec;
  // mode:
  // +128 = attach all remaining balance
  // +64 = attach all remaining balance of the inbound message
  // +32 = delete smart contract if balance becomes zero
  // +1 = pay message fees
  // +2 = skip if message cannot be sent
  // +16 = bounce if action fails
  vm::CellSlice cs{cs0};
  if (!tlb::unpack_exact(cs, act_rec)) {
    return -1;
  }
  if ((act_rec.mode & 16) && cfg.bounce_on_fail_enabled) {
    act_rec.mode &= ~16;
    ap.need_bounce_on_fail = true;
  }
  if ((act_rec.mode & ~0xe3) || (act_rec.mode & 0xc0) == 0xc0) {
    return -1;
  }
  bool skip_invalid = (act_rec.mode & 2);
  auto check_skip_invalid = [&](unsigned error_code) -> unsigned int {
    if (skip_invalid) {
      if (cfg.message_skip_enabled) {
        ap.skipped_actions++;
      }
      return 0;
    }
    return error_code;
  };
  // try to parse suggested message in act_rec.out_msg
  td::RefInt256 fwd_fee, ihr_fee;
  block::gen::MessageRelaxed::Record msg;
  if (!tlb::type_unpack_cell(act_rec.out_msg, block::gen::t_MessageRelaxed_Any, msg)) {
    return -1;
  }
  if (!block::tlb::validate_message_relaxed_libs(act_rec.out_msg)) {
    LOG(DEBUG) << "outbound message has invalid libs in StateInit";
    return -1;
  }
  if (redoing >= 1) {
    if (msg.init->size_refs() >= 2) {
      LOG(DEBUG) << "moving the StateInit of a suggested outbound message into a separate cell";
      // init:(Maybe (Either StateInit ^StateInit))
      // transform (just (left z:StateInit)) into (just (right z:^StateInit))
      CHECK(msg.init.write().fetch_ulong(2) == 2);
      vm::CellBuilder cb;
      Ref<vm::Cell> cell;
      CHECK(cb.append_cellslice_bool(std::move(msg.init))  // StateInit
            && cb.finalize_to(cell)                        // -> ^StateInit
            && cb.store_long_bool(3, 2)                    // (just (right ... ))
            && cb.store_ref_bool(std::move(cell))          // z:^StateInit
            && cb.finalize_to(cell));
      msg.init = vm::load_cell_slice_ref(cell);
    } else {
      redoing = 2;
    }
  }
  if (redoing >= 2 && msg.body->size_ext() > 1 && msg.body->prefetch_ulong(1) == 0) {
    LOG(DEBUG) << "moving the body of a suggested outbound message into a separate cell";
    // body:(Either X ^X)
    // transform (left x:X) into (right x:^X)
    CHECK(msg.body.write().fetch_ulong(1) == 0);
    vm::CellBuilder cb;
    Ref<vm::Cell> cell;
    CHECK(cb.append_cellslice_bool(std::move(msg.body))  // X
          && cb.finalize_to(cell)                        // -> ^X
          && cb.store_long_bool(1, 1)                    // (right ... )
          && cb.store_ref_bool(std::move(cell))          // x:^X
          && cb.finalize_to(cell));
    msg.body = vm::load_cell_slice_ref(cell);
  }

  block::gen::CommonMsgInfoRelaxed::Record_int_msg_info info;
  bool ext_msg = msg.info->prefetch_ulong(1);
  if (ext_msg) {
    // ext_out_msg_info$11 constructor of CommonMsgInfoRelaxed
    block::gen::CommonMsgInfoRelaxed::Record_ext_out_msg_info erec;
    if (!tlb::csr_unpack(msg.info, erec)) {
      return -1;
    }
    if (act_rec.mode & ~3) {
      return -1;  // invalid mode for an external message
    }
    info.src = std::move(erec.src);
    info.dest = std::move(erec.dest);
    // created_lt and created_at are ignored
    info.ihr_disabled = true;
    info.bounce = false;
    info.bounced = false;
    fwd_fee = ihr_fee = td::zero_refint();
  } else {
    // int_msg_info$0 constructor
    if (!tlb::csr_unpack(msg.info, info) || !block::tlb::t_CurrencyCollection.validate_csr(info.value)) {
      return -1;
    }
    if (cfg.disable_custom_fess) {
      fwd_fee = ihr_fee = td::zero_refint();
    } else {
      fwd_fee = block::tlb::t_Grams.as_integer(info.fwd_fee);
      ihr_fee = block::tlb::t_Grams.as_integer(info.ihr_fee);
    }
  }
  // set created_at and created_lt to correct values
  info.created_at = now;
  info.created_lt = ap.end_lt;
  // always clear bounced flag
  info.bounced = false;
  // have to check source address
  // it must be either our source address, or empty
  if (!check_replace_src_addr(info.src)) {
    LOG(DEBUG) << "invalid source address in a proposed outbound message";
    return 35;  // invalid source address
  }
  bool to_mc = false;
  if (!check_rewrite_dest_addr(info.dest, cfg, &to_mc, !cfg.disable_anycast)) {
    LOG(DEBUG) << "invalid destination address in a proposed outbound message";
    return check_skip_invalid(36);  // invalid destination address
  }
  if (!ext_msg && cfg.extra_currency_v2) {
    CurrencyCollection value;
    if (!value.unpack(info.value)) {
      LOG(DEBUG) << "invalid value:ExtraCurrencies in a proposed outbound message";
      return check_skip_invalid(37);  // invalid value:CurrencyCollection
    }
    if (!CurrencyCollection::remove_zero_extra_currencies(value.extra, cfg.size_limits.max_msg_extra_currencies)) {
      LOG(DEBUG) << "invalid value:ExtraCurrencies in a proposed outbound message: too many currencies (max "
                 << cfg.size_limits.max_msg_extra_currencies << ")";
      // Dict should be valid, since it was checked in t_OutListNode.validate_ref, so error here means limit exceeded
      return check_skip_invalid(44);  // invalid value:CurrencyCollection : too many extra currencies
    }
    info.value = value.pack();
  }

  // fetch message pricing info
  const MsgPrices& msg_prices = cfg.fetch_msg_prices(to_mc || account.is_masterchain());
  // If action fails, account is required to pay fine_per_cell for every visited cell
  // Number of visited cells is limited depending on available funds
  unsigned max_cells = cfg.size_limits.max_msg_cells;
  td::uint64 fine_per_cell = 0;
  if (cfg.action_fine_enabled && !account.is_special) {
    fine_per_cell = (msg_prices.cell_price >> 16) / 4;
    td::RefInt256 funds = ap.remaining_balance.grams;
    if (!ext_msg && !(act_rec.mode & 0x80) && !(act_rec.mode & 1)) {
      if (!block::tlb::t_CurrencyCollection.validate_csr(info.value)) {
        LOG(DEBUG) << "invalid value:CurrencyCollection in proposed outbound message";
        return check_skip_invalid(37);
      }
      block::CurrencyCollection value;
      CHECK(value.unpack(info.value));
      CHECK(value.grams.not_null());
      td::RefInt256 new_funds = value.grams;
      if (act_rec.mode & 0x40) {
        if (msg_balance_remaining.is_valid()) {
          new_funds += msg_balance_remaining.grams;
        }
        if (compute_phase) {
          new_funds -= compute_phase->gas_fees;
        }
        new_funds -= ap.action_fine;
        if (new_funds->sgn() < 0) {
          LOG(DEBUG)
              << "not enough value to transfer with the message: all of the inbound message value has been consumed";
          return check_skip_invalid(37);
        }
      }
      funds = std::min(funds, new_funds);
    }
    if (funds->cmp(max_cells * fine_per_cell) < 0) {
      max_cells = static_cast<unsigned>((funds / td::make_refint(fine_per_cell))->to_long());
    }
  }
  // compute size of message
  vm::CellStorageStat sstat(max_cells);  // for message size
  // preliminary storage estimation of the resulting message
  unsigned max_merkle_depth = 0;
  auto add_used_storage = [&](const auto& x, unsigned skip_root_count) -> td::Status {
    if (x.not_null()) {
      TRY_RESULT(res, sstat.add_used_storage(x, true, skip_root_count));
      max_merkle_depth = std::max(max_merkle_depth, res.max_merkle_depth);
    }
    return td::Status::OK();
  };
  add_used_storage(msg.init, 3);  // message init
  add_used_storage(msg.body, 3);  // message body (the root cell itself is not counted)
  if (!ext_msg && !cfg.extra_currency_v2) {
    add_used_storage(info.value->prefetch_ref(), 0);
  }
  auto collect_fine = [&] {
    if (cfg.action_fine_enabled && !account.is_special) {
      td::uint64 fine = fine_per_cell * std::min<td::uint64>(max_cells, sstat.cells);
      if (ap.remaining_balance.grams->cmp(fine) < 0) {
        fine = ap.remaining_balance.grams->to_long();
      }
      ap.action_fine += fine;
      ap.remaining_balance.grams -= fine;
    }
  };
  if (sstat.cells > max_cells && max_cells < cfg.size_limits.max_msg_cells) {
    LOG(DEBUG) << "not enough funds to process a message (max_cells=" << max_cells << ")";
    collect_fine();
    return check_skip_invalid(40);
  }
  if (sstat.bits > cfg.size_limits.max_msg_bits || sstat.cells > max_cells) {
    LOG(DEBUG) << "message too large, invalid";
    collect_fine();
    return check_skip_invalid(40);
  }
  if (max_merkle_depth > max_allowed_merkle_depth) {
    LOG(DEBUG) << "message has too big merkle depth, invalid";
    collect_fine();
    return check_skip_invalid(40);
  }
  LOG(DEBUG) << "storage paid for a message: " << sstat.cells << " cells, " << sstat.bits << " bits";

  // compute forwarding fees
  auto fees_c = msg_prices.compute_fwd_ihr_fees(sstat.cells, sstat.bits, info.ihr_disabled);
  LOG(DEBUG) << "computed fwd fees = " << fees_c.first << " + " << fees_c.second;

  if (account.is_special) {
    LOG(DEBUG) << "computed fwd fees set to zero for special account";
    fees_c.first = fees_c.second = 0;
  }

  // set fees to computed values
  if (fwd_fee->unsigned_fits_bits(63) && fwd_fee->to_long() < (long long)fees_c.first) {
    fwd_fee = td::make_refint(fees_c.first);
  }
  if (fees_c.second && ihr_fee->unsigned_fits_bits(63) && ihr_fee->to_long() < (long long)fees_c.second) {
    ihr_fee = td::make_refint(fees_c.second);
  }

  Ref<vm::Cell> new_msg;
  td::RefInt256 fees_collected, fees_total;
  unsigned new_msg_bits;

  if (!ext_msg) {
    // Process outbound internal message
    // check value, check/compute ihr_fees, fwd_fees
    // ...
    if (!block::tlb::t_CurrencyCollection.validate_csr(info.value)) {
      LOG(DEBUG) << "invalid value:CurrencyCollection in proposed outbound message";
      collect_fine();
      return check_skip_invalid(37);
    }
    if (info.ihr_disabled) {
      // if IHR is disabled, IHR fees will be always zero
      ihr_fee = td::zero_refint();
    }
    // extract value to be carried by the message
    block::CurrencyCollection req;
    CHECK(req.unpack(info.value));
    CHECK(req.grams.not_null());

    if (act_rec.mode & 0x80) {
      // attach all remaining balance to this message
      if (cfg.extra_currency_v2) {
        req.grams = ap.remaining_balance.grams;
      } else {
        req = ap.remaining_balance;
      }
      act_rec.mode &= ~1;  // pay fees from attached value
    } else if (act_rec.mode & 0x40) {
      // attach all remaining balance of the inbound message (in addition to the original value)
      if (cfg.extra_currency_v2) {
        req.grams += msg_balance_remaining.grams;
      } else {
        req += msg_balance_remaining;
      }
      if (!(act_rec.mode & 1)) {
        req -= ap.action_fine;
        if (compute_phase) {
          req -= compute_phase->gas_fees;
        }
        if (!req.is_valid()) {
          LOG(DEBUG)
              << "not enough value to transfer with the message: all of the inbound message value has been consumed";
          collect_fine();
          return check_skip_invalid(37);
        }
      }
    }

    // compute req_grams + fees
    td::RefInt256 req_grams_brutto = req.grams;
    fees_total = fwd_fee + ihr_fee;
    if (act_rec.mode & 1) {
      // we are going to pay the fees
      req_grams_brutto += fees_total;
    } else if (req.grams < fees_total) {
      // receiver pays the fees (but cannot)
      LOG(DEBUG) << "not enough value attached to the message to pay forwarding fees : have " << req.grams << ", need "
                 << fees_total;
      collect_fine();
      return check_skip_invalid(37);  // not enough grams
    } else {
      // decrease message value
      req.grams -= fees_total;
    }

    // check that we have at least the required value
    if (ap.remaining_balance.grams < req_grams_brutto) {
      LOG(DEBUG) << "not enough grams to transfer with the message : remaining balance is "
                 << ap.remaining_balance.to_str() << ", need " << req_grams_brutto << " (including forwarding fees)";
      collect_fine();
      return check_skip_invalid(37);  // not enough grams
    }

    if (cfg.extra_currency_v2 && !req.check_extra_currency_limit(cfg.size_limits.max_msg_extra_currencies)) {
      LOG(DEBUG) << "too many extra currencies in the message : max " << cfg.size_limits.max_msg_extra_currencies;
      return check_skip_invalid(44);  // to many extra currencies
    }

    Ref<vm::Cell> new_extra;

    if (!block::sub_extra_currency(ap.remaining_balance.extra, req.extra, new_extra)) {
      LOG(DEBUG) << "not enough extra currency to send with the message: "
                 << block::CurrencyCollection{0, req.extra}.to_str() << " required, only "
                 << block::CurrencyCollection{0, ap.remaining_balance.extra}.to_str() << " available";
      collect_fine();
      return check_skip_invalid(38);  // not enough (extra) funds
    }
    if (ap.remaining_balance.extra.not_null() || req.extra.not_null()) {
      LOG(DEBUG) << "subtracting extra currencies: "
                 << block::CurrencyCollection{0, ap.remaining_balance.extra}.to_str() << " minus "
                 << block::CurrencyCollection{0, req.extra}.to_str() << " equals "
                 << block::CurrencyCollection{0, new_extra}.to_str();
    }

    auto fwd_fee_mine = msg_prices.get_first_part(fwd_fee);
    auto fwd_fee_remain = fwd_fee - fwd_fee_mine;

    // re-pack message value
    CHECK(req.pack_to(info.value));
    CHECK(block::tlb::t_Grams.pack_integer(info.fwd_fee, fwd_fee_remain));
    CHECK(block::tlb::t_Grams.pack_integer(info.ihr_fee, ihr_fee));

    // serialize message
    CHECK(tlb::csr_pack(msg.info, info));
    vm::CellBuilder cb;
    if (!tlb::type_pack(cb, block::gen::t_MessageRelaxed_Any, msg)) {
      LOG(DEBUG) << "outbound message does not fit into a cell after rewriting";
      if (redoing == 2) {
        collect_fine();
        return check_skip_invalid(39);
      }
      return -2;
    }

    new_msg_bits = cb.size();
    new_msg = cb.finalize();

    // clear msg_balance_remaining if it has been used
    if (act_rec.mode & 0xc0) {
      if (cfg.extra_currency_v2) {
        msg_balance_remaining.grams = td::zero_refint();
      } else {
        msg_balance_remaining.set_zero();
      }
    }

    // update balance
    ap.remaining_balance -= req_grams_brutto;
    ap.remaining_balance.extra = std::move(new_extra);
    CHECK(ap.remaining_balance.is_valid());
    CHECK(ap.remaining_balance.grams->sgn() >= 0);
    fees_total = fwd_fee + ihr_fee;
    fees_collected = fwd_fee_mine;
  } else {
    // external messages also have forwarding fees
    if (ap.remaining_balance.grams < fwd_fee) {
      LOG(DEBUG) << "not enough funds to pay for an outbound external message";
      collect_fine();
      return check_skip_invalid(37);  // not enough grams
    }
    // repack message
    // ext_out_msg_info$11 constructor of CommonMsgInfo
    block::gen::CommonMsgInfo::Record_ext_out_msg_info erec;
    erec.src = info.src;
    erec.dest = info.dest;
    erec.created_at = info.created_at;
    erec.created_lt = info.created_lt;
    CHECK(tlb::csr_pack(msg.info, erec));
    vm::CellBuilder cb;
    if (!tlb::type_pack(cb, block::gen::t_MessageRelaxed_Any, msg)) {
      LOG(DEBUG) << "outbound message does not fit into a cell after rewriting";
      if (redoing == 2) {
        collect_fine();
        return check_skip_invalid(39);
      }
      return -2;
    }

    new_msg_bits = cb.size();
    new_msg = cb.finalize();

    // update balance
    ap.remaining_balance -= fwd_fee;
    CHECK(ap.remaining_balance.is_valid());
    CHECK(td::sgn(ap.remaining_balance.grams) >= 0);
    fees_collected = fees_total = fwd_fee;
  }

  if (!block::tlb::t_Message.validate_ref(new_msg)) {
    LOG(ERROR) << "generated outbound message is not a valid (Message Any) according to hand-written check";
    collect_fine();
    return -1;
  }
  if (!block::gen::t_Message_Any.validate_ref(new_msg)) {
    LOG(ERROR) << "generated outbound message is not a valid (Message Any) according to automated check";
    FLOG(INFO) {
      block::gen::t_Message_Any.print_ref(sb, new_msg);
      vm::load_cell_slice(new_msg).print_rec(sb);
    };
    collect_fine();
    return -1;
  }
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "converted outbound message: ";
      block::gen::t_Message_Any.print_ref(sb, new_msg);
    };
  }

  ap.msgs_created++;
  ap.end_lt++;

  ap.out_msgs.push_back(std::move(new_msg));
  ap.total_action_fees += fees_collected;
  ap.total_fwd_fees += fees_total;

  if ((act_rec.mode & 0xa0) == 0xa0) {
    if (cfg.extra_currency_v2) {
      CHECK(ap.remaining_balance.grams->sgn() == 0);
      ap.acc_delete_req = ap.reserved_balance.grams->sgn() == 0;
    } else {
      CHECK(ap.remaining_balance.is_zero());
      ap.acc_delete_req = ap.reserved_balance.is_zero();
    }
  }

  ap.tot_msg_bits += sstat.bits + new_msg_bits;
  ap.tot_msg_cells += sstat.cells + 1;

  return 0;
}

/**
 * Tries to reserve a currency an action phase.
 *
 * @param cs The cell slice containing the action data serialized as action_reserve_currency TLB-scheme.
 * @param ap The action phase.
 * @param cfg The action phase configuration.
 *
 * @returns 0 if the currency is successfully reserved, error code otherwise.
 */
int Transaction::try_action_reserve_currency(vm::CellSlice& cs, ActionPhase& ap, const ActionPhaseConfig& cfg) {
  block::gen::OutAction::Record_action_reserve_currency rec;
  if (!tlb::unpack_exact(cs, rec)) {
    return -1;
  }
  if ((rec.mode & 16) && cfg.bounce_on_fail_enabled) {
    rec.mode &= ~16;
    ap.need_bounce_on_fail = true;
  }
  if (rec.mode & ~15) {
    return -1;
  }
  int mode = rec.mode;
  LOG(INFO) << "in try_action_reserve_currency(" << mode << ")";
  CurrencyCollection reserve, newc;
  if (!reserve.validate_unpack(std::move(rec.currency))) {
    LOG(DEBUG) << "cannot parse currency field in action_reserve_currency";
    return -1;
  }
  if (cfg.extra_currency_v2 && reserve.has_extra()) {
    LOG(DEBUG) << "cannot reserve extra currencies";
    return -1;
  }
  LOG(DEBUG) << "action_reserve_currency: mode=" << mode << ", reserve=" << reserve.to_str()
             << ", balance=" << ap.remaining_balance.to_str() << ", original balance=" << original_balance.to_str();
  if (mode & 4) {
    if (mode & 8) {
      if (cfg.extra_currency_v2) {
        reserve.grams = original_balance.grams - reserve.grams;
      } else {
        reserve = original_balance - reserve;
      }
    } else {
      if (cfg.extra_currency_v2) {
        reserve.grams += original_balance.grams;
      } else {
        reserve += original_balance;
      }
    }
  } else if (mode & 8) {
    LOG(DEBUG) << "invalid reserve mode " << mode;
    return -1;
  }
  if (!reserve.is_valid() || td::sgn(reserve.grams) < 0) {
    LOG(DEBUG) << "cannot reserve a negative amount: " << reserve.to_str();
    return -1;
  }
  if (mode & 2) {
    if (cfg.reserve_extra_enabled) {
      if (!reserve.clamp(ap.remaining_balance)) {
        LOG(DEBUG) << "failed to clamp reserve amount " << mode;
        return -1;
      }
    } else {
      reserve.grams = std::min(reserve.grams, ap.remaining_balance.grams);
    }
  }
  if (reserve.grams > ap.remaining_balance.grams) {
    LOG(DEBUG) << "cannot reserve " << reserve.grams << " nanograms : only " << ap.remaining_balance.grams
               << " available";
    return 37;  // not enough grams
  }
  if (!block::sub_extra_currency(ap.remaining_balance.extra, reserve.extra, newc.extra)) {
    LOG(DEBUG) << "not enough extra currency to reserve: " << block::CurrencyCollection{0, reserve.extra}.to_str()
               << " required, only " << block::CurrencyCollection{0, ap.remaining_balance.extra}.to_str()
               << " available";
    return 38;  // not enough (extra) funds
  }
  newc.grams = ap.remaining_balance.grams - reserve.grams;
  if (mode & 1) {
    // leave only res_grams, reserve everything else
    if (cfg.extra_currency_v2) {
      std::swap(newc.grams, reserve.grams);
    } else {
      std::swap(newc, reserve);
    }
  }
  // set remaining_balance to new_grams and new_extra
  ap.remaining_balance = std::move(newc);
  // increase reserved_balance by res_grams and res_extra
  ap.reserved_balance += std::move(reserve);
  CHECK(ap.reserved_balance.is_valid());
  CHECK(ap.remaining_balance.is_valid());
  LOG(INFO) << "changed remaining balance to " << ap.remaining_balance.to_str() << ", reserved balance to "
            << ap.reserved_balance.to_str();
  ap.spec_actions++;
  return 0;
}

/**
 * Calculates the number of public libraries in the dictionary.
 *
 * @param libraries The dictionary of account libraries.
 *
 * @returns The number of public libraries in the dictionary.
 */
static td::uint32 get_public_libraries_count(const td::Ref<vm::Cell>& libraries) {
  td::uint32 count = 0;
  vm::Dictionary dict{libraries, 256};
  dict.check_for_each([&](td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int) {
    if (block::is_public_library(key, std::move(value))) {
      ++count;
    }
    return true;
  });
  return count;
}

/**
 * Calculates the number of changes of public libraries in the dictionary.
 *
 * @param old_libraries The dictionary of account libraries before the transaction.
 * @param new_libraries The dictionary of account libraries after the transaction.
 *
 * @returns The number of changed public libraries.
 */
static td::uint32 get_public_libraries_diff_count(const td::Ref<vm::Cell>& old_libraries,
                                                  const td::Ref<vm::Cell>& new_libraries) {
  td::uint32 count = 0;
  vm::Dictionary dict1{old_libraries, 256};
  vm::Dictionary dict2{new_libraries, 256};
  dict1.scan_diff(dict2, [&](td::ConstBitPtr key, int n, Ref<vm::CellSlice> val1, Ref<vm::CellSlice> val2) -> bool {
    CHECK(n == 256);
    bool is_public1 = val1.not_null() && block::is_public_library(key, val1);
    bool is_public2 = val2.not_null() && block::is_public_library(key, val2);
    if (is_public1 != is_public2) {
      ++count;
    }
    return true;
  });
  return count;
}

/**
 * Checks that the new account state fits in the limits.
 * This function is not called for special accounts.
 *
 * @param size_limits The size limits configuration.
 * @param update_storage_stat Store storage stat in the Transaction's AccountStorageStat.
 *
 * @returns A `td::Status` indicating the result of the check.
 *          - If the state limits are within the allowed range, returns OK.
 *          - If the state limits exceed the maximum allowed range, returns an error.
 */
td::Status Transaction::check_state_limits(const SizeLimitsConfig& size_limits, bool update_storage_stat) {
  auto cell_equal = [](const td::Ref<vm::Cell>& a, const td::Ref<vm::Cell>& b) -> bool {
    return a.is_null() || b.is_null() ? a.is_null() == b.is_null() : a->get_hash() == b->get_hash();
  };
  if (cell_equal(account.code, new_code) && cell_equal(account.data, new_data) &&
      cell_equal(account.library, new_library)) {
    return td::Status::OK();
  }
  AccountStorageStat storage_stat;
  if (update_storage_stat && account.account_storage_stat) {
    storage_stat = AccountStorageStat{&account.account_storage_stat.value()};
  }
  {
    TD_PERF_COUNTER(transaction_storage_stat_a);
    td::Timer timer;
    TRY_STATUS(storage_stat.replace_roots({new_code, new_data, new_library}, /* check_merkle_depth = */ true));
    if (timer.elapsed() > 0.1) {
      LOG(INFO) << "Compute used storage (1) took " << timer.elapsed() << "s";
    }
  }

  if (storage_stat.get_total_cells() > size_limits.max_acc_state_cells ||
      storage_stat.get_total_bits() > size_limits.max_acc_state_bits) {
    return td::Status::Error(PSTRING() << "account state is too big: cells=" << storage_stat.get_total_cells()
                                       << ", bits=" << storage_stat.get_total_bits()
                                       << " (max cells=" << size_limits.max_acc_state_cells
                                       << ", max bits=" << size_limits.max_acc_state_bits << ")");
  }
  if (account.is_masterchain() && !cell_equal(account.library, new_library)) {
    auto libraries_count = get_public_libraries_count(new_library);
    if (libraries_count > size_limits.max_acc_public_libraries) {
      return td::Status::Error(PSTRING() << "too many public libraries: " << libraries_count << " (max "
                                         << size_limits.max_acc_public_libraries << ")");
    }
  }
  if (update_storage_stat) {
    // storage_stat will be reused in compute_state()
    new_account_storage_stat.value_force() = std::move(storage_stat);
  }
  return td::Status::OK();
}

/**
 * Prepares the bounce phase of a transaction.
 *
 * @param cfg The configuration for the action phase.
 *
 * @returns True if the bounce phase was successfully prepared, false otherwise.
 */
bool Transaction::prepare_bounce_phase(const ActionPhaseConfig& cfg) {
  if (in_msg.is_null() || !bounce_enabled) {
    return false;
  }
  bounce_phase = std::make_unique<BouncePhase>();
  BouncePhase& bp = *bounce_phase;
  block::gen::Message::Record msg;
  block::gen::CommonMsgInfo::Record_int_msg_info info;
  auto cs = vm::load_cell_slice(in_msg);
  if (!(tlb::unpack(cs, info) && gen::t_Maybe_Either_StateInit_Ref_StateInit.skip(cs) && cs.have(1) &&
        cs.have_refs((int)cs.prefetch_ulong(1)))) {
    bounce_phase.reset();
    return false;
  }
  if (cs.fetch_ulong(1)) {
    cs = vm::load_cell_slice(cs.prefetch_ref());
  }
  info.ihr_disabled = true;
  info.bounce = false;
  info.bounced = true;
  std::swap(info.src, info.dest);
  bool to_mc = false;
  if (!check_rewrite_dest_addr(info.dest, cfg, &to_mc)) {
    LOG(DEBUG) << "invalid destination address in a bounced message";
    bounce_phase.reset();
    return false;
  }
  // fetch message pricing info
  const MsgPrices& msg_prices = cfg.fetch_msg_prices(to_mc || account.is_masterchain());
  // compute size of message
  vm::CellStorageStat sstat;  // for message size
  // preliminary storage estimation of the resulting message
  sstat.compute_used_storage(info.value->prefetch_ref());
  bp.msg_bits = sstat.bits;
  bp.msg_cells = sstat.cells;
  // compute forwarding fees
  bp.fwd_fees = msg_prices.compute_fwd_fees(sstat.cells, sstat.bits);
  // check whether the message has enough funds
  auto msg_balance = msg_balance_remaining;
  if (compute_phase && compute_phase->gas_fees.not_null()) {
    msg_balance.grams -= compute_phase->gas_fees;
  }
  if (action_phase && action_phase->action_fine.not_null()) {
    msg_balance.grams -= action_phase->action_fine;
  }
  if ((msg_balance.grams < 0) ||
      (msg_balance.grams->signed_fits_bits(64) && msg_balance.grams->to_long() < (long long)bp.fwd_fees)) {
    // not enough funds
    bp.nofunds = true;
    return true;
  }
  // debit msg_balance_remaining from account's (tentative) balance
  balance -= msg_balance;
  CHECK(balance.is_valid());
  // debit total forwarding fees from the message's balance, then split forwarding fees into our part and remaining part
  msg_balance -= td::make_refint(bp.fwd_fees);
  bp.fwd_fees_collected = msg_prices.get_first_part(bp.fwd_fees);
  bp.fwd_fees -= bp.fwd_fees_collected;
  total_fees += td::make_refint(bp.fwd_fees_collected);
  // serialize outbound message
  info.created_lt = start_lt + 1 + out_msgs.size();
  end_lt++;
  info.created_at = now;
  vm::CellBuilder cb;
  CHECK(cb.store_long_bool(5, 4)                            // int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool
        && cb.append_cellslice_bool(info.src)               // src:MsgAddressInt
        && cb.append_cellslice_bool(info.dest)              // dest:MsgAddressInt
        && msg_balance.store(cb)                            // value:CurrencyCollection
        && block::tlb::t_Grams.store_long(cb, 0)            // ihr_fee:Grams
        && block::tlb::t_Grams.store_long(cb, bp.fwd_fees)  // fwd_fee:Grams
        && cb.store_long_bool(info.created_lt, 64)          // created_lt:uint64
        && cb.store_long_bool(info.created_at, 32)          // created_at:uint32
        && cb.store_bool_bool(false));                      // init:(Maybe ...)
  if (cfg.bounce_msg_body) {
    int body_bits = std::min((int)cs.size(), cfg.bounce_msg_body);
    if (cb.remaining_bits() >= body_bits + 33u) {
      CHECK(cb.store_bool_bool(false)                             // body:(Either X ^X) -> left X
            && cb.store_long_bool(-1, 32)                         // int = -1 ("message type")
            && cb.append_bitslice(cs.prefetch_bits(body_bits)));  // truncated message body
    } else {
      vm::CellBuilder cb2;
      CHECK(cb.store_bool_bool(true)                             // body:(Either X ^X) -> right ^X
            && cb2.store_long_bool(-1, 32)                       // int = -1 ("message type")
            && cb2.append_bitslice(cs.prefetch_bits(body_bits))  // truncated message body
            && cb.store_builder_ref_bool(std::move(cb2)));       // ^X
    }
  } else {
    CHECK(cb.store_bool_bool(false));  // body:(Either ..)
  }
  CHECK(cb.finalize_to(bp.out_msg));
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "generated bounced message: ";
      block::gen::t_Message_Any.print_ref(sb, bp.out_msg);
    };
  }
  out_msgs.push_back(bp.out_msg);
  bp.ok = true;
  return true;
}
}  // namespace transaction

/*
 * 
 *  SERIALIZE PREPARED TRANSACTION
 * 
 */

/**
 * Stores the account status in a CellBuilder object.
 *
 * @param cb The CellBuilder object to store the account status in.
 * @param acc_status The account status to store.
 *
 * @returns True if the account status was successfully stored, false otherwise.
 */
bool Account::store_acc_status(vm::CellBuilder& cb, int acc_status) const {
  int v;
  switch (acc_status) {
    case acc_nonexist:
    case acc_deleted:
      v = 3;  // acc_state_nonexist$11
      break;
    case acc_uninit:
      v = 0;  // acc_state_uninit$00
      break;
    case acc_frozen:
      v = 1;  // acc_state_frozen$01
      break;
    case acc_active:
      v = 2;  // acc_state_active$10
      break;
    default:
      return false;
  }
  return cb.store_long_bool(v, 2);
}

/**
 * Removes extra currencies dict from AccountStorage.
 *
 * This is used for computing account storage stats.
 *
 * @param storage_cs AccountStorage as CellSlice.
 *
 * @returns AccountStorage without extra currencies as CellSlice.
 */
static td::Ref<vm::CellSlice> storage_without_extra_currencies(td::Ref<vm::CellSlice> storage_cs) {
  block::gen::AccountStorage::Record rec;
  if (!block::gen::csr_unpack(storage_cs, rec)) {
    LOG(ERROR) << "failed to unpack AccountStorage";
    return {};
  }
  if (rec.balance->size_refs() > 0) {
    block::gen::CurrencyCollection::Record balance;
    if (!block::gen::csr_unpack(rec.balance, balance)) {
      LOG(ERROR) << "failed to unpack AccountStorage";
      return {};
    }
    balance.other = vm::CellBuilder{}.store_zeroes(1).as_cellslice_ref();
    if (!block::gen::csr_pack(rec.balance, balance)) {
      LOG(ERROR) << "failed to pack AccountStorage";
      return {};
    }
  }
  td::Ref<vm::CellSlice> result;
  if (!block::gen::csr_pack(result, rec)) {
    LOG(ERROR) << "failed to pack AccountStorage";
    return {};
  }
  return result;
}

namespace transaction {
/**
 * Computes the new state of the account.
 *
 * @param cfg The configuration for the serialization phase.
 *
 * @returns True if the state computation is successful, false otherwise.
 */
bool Transaction::compute_state(const SerializeConfig& cfg) {
  if (new_total_state.not_null()) {
    return true;
  }
  if (acc_status == Account::acc_uninit && !was_activated && balance.is_zero()) {
    LOG(DEBUG) << "account is uninitialized and has zero balance, deleting it back";
    acc_status = Account::acc_nonexist;
    was_created = false;
  }
  if (acc_status == Account::acc_deleted && !balance.is_zero()) {
    acc_status = Account::acc_uninit;
  }
  if (acc_status == Account::acc_nonexist || acc_status == Account::acc_deleted) {
    CHECK(balance.is_zero());
    vm::CellBuilder cb;
    CHECK(cb.store_long_bool(0, 1)  // account_none$0
          && cb.finalize_to(new_total_state));
    return true;
  }
  vm::CellBuilder cb;
  CHECK(cb.store_long_bool(end_lt, 64)  // account_storage$_ last_trans_lt:uint64
        && balance.store(cb));          // balance:CurrencyCollection
  int ticktock = new_tick * 2 + new_tock;
  unsigned si_pos = 0;
  int fixed_prefix_length = cfg.disable_anycast ? new_fixed_prefix_length : account.addr_rewrite_length;
  if (acc_status == Account::acc_uninit) {
    CHECK(cb.store_long_bool(0, 2));  // account_uninit$00 = AccountState
  } else if (acc_status == Account::acc_frozen) {
    if (was_frozen) {
      vm::CellBuilder cb2;
      CHECK(fixed_prefix_length ? cb2.store_long_bool(fixed_prefix_length + 32, 6)  // _ ... = StateInit
                                : cb2.store_long_bool(0, 1));  // ... fixed_prefix_length:(Maybe (## 5))
      CHECK(ticktock ? cb2.store_long_bool(ticktock | 4, 3) : cb2.store_long_bool(0, 1));  // special:(Maybe TickTock)
      CHECK(cb2.store_maybe_ref(new_code) && cb2.store_maybe_ref(new_data) && cb2.store_maybe_ref(new_library));
      // code:(Maybe ^Cell) data:(Maybe ^Cell) library:(HashmapE 256 SimpleLib)
      auto frozen_state = cb2.finalize();
      frozen_hash = frozen_state->get_hash().bits();
      if (verbosity >= 3 * 1) {  // !!!DEBUG!!!
        FLOG(INFO) {
          sb << "freezing state of smart contract: ";
          block::gen::t_StateInit.print_ref(sb, frozen_state);
          CHECK(block::gen::t_StateInit.validate_ref(frozen_state));
          CHECK(block::tlb::t_StateInit.validate_ref(frozen_state));
          sb << "with hash " << frozen_hash.to_hex();
        };
      }
    }
    new_code.clear();
    new_data.clear();
    new_library.clear();
    if (frozen_hash == account.addr_orig) {
      // if frozen_hash equals account's "original" address (before rewriting), do not need storing hash
      CHECK(cb.store_long_bool(0, 2));  // account_uninit$00 = AccountState
    } else {
      CHECK(cb.store_long_bool(1, 2)              // account_frozen$01
            && cb.store_bits_bool(frozen_hash));  // state_hash:bits256
    }
  } else {
    CHECK(acc_status == Account::acc_active && !was_frozen && !was_deleted);
    si_pos = cb.size_ext() + 1;
    CHECK(fixed_prefix_length ? cb.store_long_bool(fixed_prefix_length + 96, 7)  // account_active$1 _:StateInit
                              : cb.store_long_bool(2, 2));  // ... fixed_prefix_length:(Maybe (## 5))
    CHECK(ticktock ? cb.store_long_bool(ticktock | 4, 3) : cb.store_long_bool(0, 1));  // special:(Maybe TickTock)
    CHECK(cb.store_maybe_ref(new_code) && cb.store_maybe_ref(new_data) && cb.store_maybe_ref(new_library));
    // code:(Maybe ^Cell) data:(Maybe ^Cell) library:(HashmapE 256 SimpleLib)
  }
  auto storage = cb.finalize();
  new_storage = td::Ref<vm::CellSlice>(true, vm::NoVm(), storage);
  if (si_pos) {
    auto cs_ref = load_cell_slice_ref(storage);
    CHECK(cs_ref.unique_write().skip_ext(si_pos));
    new_inner_state = std::move(cs_ref);
  } else {
    new_inner_state.clear();
  }

  td::Ref<vm::CellSlice> old_storage_for_stat = account.storage;
  td::Ref<vm::CellSlice> new_storage_for_stat = new_storage;
  if (cfg.extra_currency_v2) {
    new_storage_for_stat = storage_without_extra_currencies(new_storage);
    if (new_storage_for_stat.is_null()) {
      return false;
    }
    if (old_storage_for_stat.not_null()) {
      old_storage_for_stat = storage_without_extra_currencies(old_storage_for_stat);
      if (old_storage_for_stat.is_null()) {
        return false;
      }
    }
  } else if (cfg.store_storage_dict_hash) {
    LOG(ERROR) << "unsupported store_storage_dict_hash=true, extra_currency_v2=false";
    return false;
  }

  bool storage_refs_changed = false;
  if (old_storage_for_stat.is_null() || new_storage_for_stat->size_refs() != old_storage_for_stat->size_refs()) {
    storage_refs_changed = true;
  } else {
    for (unsigned i = 0; i < new_storage_for_stat->size_refs(); i++) {
      if (new_storage_for_stat->prefetch_ref(i)->get_hash() != old_storage_for_stat->prefetch_ref(i)->get_hash()) {
        storage_refs_changed = true;
        break;
      }
    }
  }

  bool store_storage_dict_hash = cfg.store_storage_dict_hash && !account.is_masterchain();
  if (storage_refs_changed ||
      (store_storage_dict_hash && !account.storage_dict_hash && account.storage_used.cells > 25)) {
    TD_PERF_COUNTER(transaction_storage_stat_b);
    td::Timer timer;
    if (!new_account_storage_stat && account.account_storage_stat) {
      new_account_storage_stat = AccountStorageStat(&account.account_storage_stat.value());
    }
    AccountStorageStat& stats = new_account_storage_stat.value_force();
    // Don't check Merkle depth and size here - they were checked in check_state_limits
    td::Status S = stats.replace_roots(new_storage_for_stat->prefetch_all_refs());
    if (S.is_error()) {
      LOG(ERROR) << "Cannot recompute storage stats for account " << account.addr.to_hex() << ": " << S.move_as_error();
      return false;
    }
    // Root of AccountStorage is not counted in AccountStorageStat
    new_storage_used.cells = stats.get_total_cells() + 1;
    new_storage_used.bits = stats.get_total_bits() + new_storage_for_stat->size();
    // TODO: think about this limit (25)
    if (store_storage_dict_hash && new_storage_used.cells > 25) {
      auto r_hash = stats.get_dict_hash();
      if (r_hash.is_error()) {
        LOG(ERROR) << "Cannot compute storage dict hash for account " << account.addr.to_hex() << ": "
                   << r_hash.move_as_error();
        return false;
      }
      new_storage_dict_hash = r_hash.move_as_ok();
    }
    if (timer.elapsed() > 0.1) {
      LOG(INFO) << "Compute used storage (2) took " << timer.elapsed() << "s";
    }
  } else {
    new_storage_used = account.storage_used;
    new_storage_used.bits -= old_storage_for_stat->size();
    new_storage_used.bits += new_storage_for_stat->size();
    new_account_storage_stat = {};
    if (store_storage_dict_hash) {
      new_storage_dict_hash = account.storage_dict_hash;
    }
  }

  CHECK(cb.store_long_bool(1, 1)                                                      // account$1
        && cb.append_cellslice_bool(cfg.disable_anycast ? my_addr : account.my_addr)  // addr:MsgAddressInt
        && block::store_UInt7(cb, new_storage_used.cells)        // storage_used$_ cells:(VarUInteger 7)
        && block::store_UInt7(cb, new_storage_used.bits)         //   bits:(VarUInteger 7)
        && cb.store_long_bool(new_storage_dict_hash ? 1 : 0, 3)  // extra:StorageExtraInfo
        && (!new_storage_dict_hash || cb.store_bits_bool(new_storage_dict_hash.value()))  // dict_hash:uint256
        && cb.store_long_bool(last_paid, 32));                                            // last_paid:uint32
  if (due_payment.not_null() && td::sgn(due_payment) != 0) {
    CHECK(cb.store_long_bool(1, 1) && block::tlb::t_Grams.store_integer_ref(cb, due_payment));
    // due_payment:(Maybe Grams)
  } else {
    CHECK(cb.store_long_bool(0, 1));
  }
  CHECK(cb.append_cellslice_bool(new_storage));
  new_total_state = cb.finalize();
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "new account state: ";
      block::gen::t_Account.print_ref(sb, new_total_state);
    };
  }
  CHECK(block::tlb::t_Account.validate_ref(new_total_state));
  return true;
}

/**
 * Serializes the transaction object using Transaction TLB-scheme.
 * 
 * Updates root.
 *
 * @param cfg The configuration for the serialization.
 *
 * @returns True if the serialization is successful, False otherwise.
 */
bool Transaction::serialize(const SerializeConfig& cfg) {
  if (root.not_null()) {
    return true;
  }
  if (!compute_state(cfg)) {
    return false;
  }
  vm::Dictionary dict{15};
  for (unsigned i = 0; i < out_msgs.size(); i++) {
    td::BitArray<15> key{i};
    if (!dict.set_ref(key, out_msgs[i], vm::Dictionary::SetMode::Add)) {
      return false;
    }
  }
  vm::CellBuilder cb, cb2;
  if (!(cb.store_long_bool(7, 4)                                             // transaction$0111
        && cb.store_bits_bool(account.addr)                                  // account_addr:bits256
        && cb.store_long_bool(start_lt)                                      // lt:uint64
        && cb.store_bits_bool(account.last_trans_hash_)                      // prev_trans_hash:bits256
        && cb.store_long_bool(account.last_trans_lt_, 64)                    // prev_trans_lt:uint64
        && cb.store_long_bool(account.now_, 32)                              // now:uint32
        && cb.store_ulong_rchk_bool(out_msgs.size(), 15)                     // outmsg_cnt:uint15
        && account.store_acc_status(cb)                                      // orig_status:AccountStatus
        && account.store_acc_status(cb, acc_status)                          // end_status:AccountStatus
        && cb2.store_maybe_ref(in_msg)                                       // ^[ in_msg:(Maybe ^(Message Any)) ...
        && std::move(dict).append_dict_to_bool(cb2)                          //    out_msgs:(HashmapE 15 ^(Message Any))
        && cb.store_ref_bool(cb2.finalize())                                 // ]
        && total_fees.store(cb)                                              // total_fees:CurrencyCollection
        && cb2.store_long_bool(0x72, 8)                                      //   update_hashes#72
        && cb2.store_bits_bool(account.total_state->get_hash().bits(), 256)  //   old_hash:bits256
        && cb2.store_bits_bool(new_total_state->get_hash().bits(), 256)      //   new_hash:bits256
        && cb.store_ref_bool(cb2.finalize()))) {                             // state_update:^(HASH_UPDATE Account)
    return false;
  }

  switch (trans_type) {
    case tr_tick:  // fallthrough
    case tr_tock: {
      vm::CellBuilder cb3;
      bool act = compute_phase->success;
      bool act_ok = act && action_phase->success;
      CHECK(cb2.store_long_bool(trans_type == tr_tick ? 2 : 3, 4)  // trans_tick_tock$000 is_tock:Bool
            && serialize_storage_phase(cb2)                        // storage:TrStoragePhase
            && serialize_compute_phase(cb2)                        // compute_ph:TrComputePhase
            && cb2.store_bool_bool(act)                            // action:(Maybe
            && (!act || (serialize_action_phase(cb3)               //   ^TrActionPhase)
                         && cb2.store_ref_bool(cb3.finalize()))) &&
            cb2.store_bool_bool(!act_ok)         // aborted:Bool
            && cb2.store_bool_bool(was_deleted)  // destroyed:Bool
            && cb.store_ref_bool(cb2.finalize()) && cb.finalize_to(root));
      break;
    }
    case tr_ord: {
      vm::CellBuilder cb3;
      bool have_storage = (bool)storage_phase;
      bool have_credit = (bool)credit_phase;
      bool have_bounce = (bool)bounce_phase;
      bool act = compute_phase->success;
      bool act_ok = act && action_phase->success;
      CHECK(cb2.store_long_bool(0, 4)                           // trans_ord$0000
            && cb2.store_long_bool(!bounce_enabled, 1)          // credit_first:Bool
            && cb2.store_bool_bool(have_storage)                // storage_ph:(Maybe
            && (!have_storage || serialize_storage_phase(cb2))  //   TrStoragePhase)
            && cb2.store_bool_bool(have_credit)                 // credit_ph:(Maybe
            && (!have_credit || serialize_credit_phase(cb2))    //   TrCreditPhase)
            && serialize_compute_phase(cb2)                     // compute_ph:TrComputePhase
            && cb2.store_bool_bool(act)                         // action:(Maybe
            && (!act || (serialize_action_phase(cb3) && cb2.store_ref_bool(cb3.finalize())))  //   ^TrActionPhase)
            && cb2.store_bool_bool(!act_ok)                                                   // aborted:Bool
            && cb2.store_bool_bool(have_bounce)                                               // bounce:(Maybe
            && (!have_bounce || serialize_bounce_phase(cb2))                                  //   TrBouncePhase
            && cb2.store_bool_bool(was_deleted)                                               // destroyed:Bool
            && cb.store_ref_bool(cb2.finalize()) && cb.finalize_to(root));
      break;
    }
    default:
      return false;
  }
  if (verbosity >= 3 * 1) {
    FLOG(INFO) {
      sb << "new transaction: ";
      block::gen::t_Transaction.print_ref(sb, root);
      vm::load_cell_slice(root).print_rec(sb);
    };
  }

  if (!block::gen::t_Transaction.validate_ref(4096, root)) {
    LOG(ERROR) << "newly-generated transaction failed to pass automated validation:";
    FLOG(INFO) {
      vm::load_cell_slice(root).print_rec(sb);
      block::gen::t_Transaction.print_ref(sb, root);
    };
    root.clear();
    return false;
  }
  if (!block::tlb::t_Transaction.validate_ref(4096, root)) {
    LOG(ERROR) << "newly-generated transaction failed to pass hand-written validation:";
    FLOG(INFO) {
      vm::load_cell_slice(root).print_rec(sb);
      block::gen::t_Transaction.print_ref(sb, root);
    };
    root.clear();
    return false;
  }

  return true;
}

/**
 * Serializes the storage phase of a transaction.
 *
 * @param cb The CellBuilder to store the serialized data.
 *
 * @returns True if the serialization is successful, false otherwise.
 */
bool Transaction::serialize_storage_phase(vm::CellBuilder& cb) {
  if (!storage_phase) {
    return false;
  }
  StoragePhase& sp = *storage_phase;
  bool ok;
  // tr_phase_storage$_ storage_fees_collected:Grams
  if (sp.fees_collected.not_null()) {
    ok = block::tlb::t_Grams.store_integer_ref(cb, sp.fees_collected);
  } else {
    ok = block::tlb::t_Grams.null_value(cb);
  }
  // storage_fees_due:(Maybe Grams)
  ok &= block::store_Maybe_Grams_nz(cb, sp.fees_due);
  // status_change:AccStatusChange
  if (sp.deleted || sp.frozen) {
    ok &= cb.store_long_bool(sp.deleted ? 3 : 2, 2);  // acst_frozen$10 acst_deleted$11
  } else {
    ok &= cb.store_long_bool(0, 1);  // acst_unchanged$0 = AccStatusChange
  }
  return ok;
}

/**
 * Serializes the credit phase of a transaction.
 *
 * @param cb The CellBuilder to store the serialized data.
 *
 * @returns True if the credit phase was successfully serialized, false otherwise.
 */
bool Transaction::serialize_credit_phase(vm::CellBuilder& cb) {
  if (!credit_phase) {
    return false;
  }
  CreditPhase& cp = *credit_phase;
  // tr_phase_credit$_ due_fees_collected:(Maybe Grams) credit:CurrencyCollection
  return block::store_Maybe_Grams_nz(cb, cp.due_fees_collected) && cp.credit.store(cb);
}

/**
 * Serializes the compute phase of a transaction.
 *
 * @param cb The CellBuilder to store the serialized data.
 *
 * @returns True if the serialization was successful, false otherwise.
 */
bool Transaction::serialize_compute_phase(vm::CellBuilder& cb) {
  if (!compute_phase) {
    return false;
  }
  ComputePhase& cp = *compute_phase;
  switch (cp.skip_reason) {
    // tr_compute_phase_skipped$0 reason:ComputeSkipReason;
    case ComputePhase::sk_no_state:
      return cb.store_long_bool(0, 3);  // cskip_no_state$00 = ComputeSkipReason;
    case ComputePhase::sk_bad_state:
      return cb.store_long_bool(1, 3);  // cskip_bad_state$01 = ComputeSkipReason;
    case ComputePhase::sk_no_gas:
      return cb.store_long_bool(2, 3);  // cskip_no_gas$10 = ComputeSkipReason;
    case ComputePhase::sk_suspended:
      return cb.store_long_bool(0b0110, 4);  // cskip_suspended$110 = ComputeSkipReason;
    case ComputePhase::sk_none:
      break;
    default:
      return false;
  }
  vm::CellBuilder cb2;
  bool ok, credit = (cp.gas_credit != 0), exarg = (cp.exit_arg != 0);
  ok = cb.store_long_bool(1, 1)                                   // tr_phase_compute_vm$1
       && cb.store_long_bool(cp.success, 1)                       // success:Bool
       && cb.store_long_bool(cp.msg_state_used, 1)                // msg_state_used:Bool
       && cb.store_long_bool(cp.account_activated, 1)             // account_activated:Bool
       && block::tlb::t_Grams.store_integer_ref(cb, cp.gas_fees)  // gas_fees:Grams
       && block::store_UInt7(cb2, cp.gas_used)                    // ^[ gas_used:(VarUInteger 7)
       && block::store_UInt7(cb2, cp.gas_limit)                   //    gas_limit:(VarUInteger 7)
       && cb2.store_long_bool(credit, 1)                          //    gas_credit:(Maybe (VarUInteger 3))
       && (!credit || block::tlb::t_VarUInteger_3.store_long(cb2, cp.gas_credit)) &&
       cb2.store_long_rchk_bool(cp.mode, 8)      //    mode:int8
       && cb2.store_long_bool(cp.exit_code, 32)  //    exit_code:int32
       && cb2.store_long_bool(exarg, 1)          //    exit_arg:(Maybe int32)
       && (!exarg || cb2.store_long_bool(cp.exit_arg, 32)) &&
       cb2.store_ulong_rchk_bool(cp.vm_steps, 32)      //    vm_steps:uint32
       && cb2.store_bits_bool(cp.vm_init_state_hash)   //    vm_init_state_hash:bits256
       && cb2.store_bits_bool(cp.vm_final_state_hash)  //    vm_final_state_hash:bits256
       && cb.store_ref_bool(cb2.finalize());           // ] = TrComputePhase
  return ok;
}

/**
 * Serializes the action phase of a transaction.
 *
 * @param cb The CellBuilder to store the serialized data.
 *
 * @returns True if the serialization is successful, false otherwise.
 */
bool Transaction::serialize_action_phase(vm::CellBuilder& cb) {
  if (!action_phase) {
    return false;
  }
  ActionPhase& ap = *action_phase;
  bool ok, arg = (ap.result_arg != 0);
  ok = cb.store_long_bool(ap.success, 1)                                             // tr_phase_action$_ success:Bool
       && cb.store_long_bool(ap.valid, 1)                                            // valid:Bool
       && cb.store_long_bool(ap.no_funds, 1)                                         // no_funds:Bool
       && cb.store_long_bool(ap.acc_status_change, (ap.acc_status_change >> 1) + 1)  // status_change:AccStatusChange
       && block::store_Maybe_Grams_nz(cb, ap.total_fwd_fees)                         // total_fwd_fees:(Maybe Grams)
       && block::store_Maybe_Grams_nz(cb, ap.total_action_fees)                      // total_action_fees:(Maybe Grams)
       && cb.store_long_bool(ap.result_code, 32)                                     // result_code:int32
       && cb.store_long_bool(arg, 1)                                                 // result_arg:(Maybe
       && (!arg || cb.store_long_bool(ap.result_arg, 32))                            //    uint32)
       && cb.store_ulong_rchk_bool(ap.tot_actions, 16)                               // tot_actions:uint16
       && cb.store_ulong_rchk_bool(ap.spec_actions, 16)                              // spec_actions:uint16
       && cb.store_ulong_rchk_bool(ap.skipped_actions, 16)                           // skipped_actions:uint16
       && cb.store_ulong_rchk_bool(ap.msgs_created, 16)                              // msgs_created:uint16
       && cb.store_bits_bool(ap.action_list_hash)                                    // action_list_hash:bits256
       && block::store_UInt7(cb, ap.tot_msg_cells, ap.tot_msg_bits);                 // tot_msg_size:StorageUsed
  return ok;
}

/**
 * Serializes the bounce phase of a transaction.
 *
 * @param cb The CellBuilder to store the serialized data.
 *
 * @returns True if the bounce phase was successfully serialized, false otherwise.
 */
bool Transaction::serialize_bounce_phase(vm::CellBuilder& cb) {
  if (!bounce_phase) {
    return false;
  }
  BouncePhase& bp = *bounce_phase;
  if (!(bp.ok ^ bp.nofunds)) {
    return false;
  }
  if (bp.nofunds) {
    return cb.store_long_bool(1, 2)                              // tr_phase_bounce_nofunds$01
           && block::store_UInt7(cb, bp.msg_cells, bp.msg_bits)  // msg_size:StorageUsed
           && block::tlb::t_Grams.store_long(cb, bp.fwd_fees);   // req_fwd_fees:Grams
  } else {
    return cb.store_long_bool(1, 1)                                      // tr_phase_bounce_ok$1
           && block::store_UInt7(cb, bp.msg_cells, bp.msg_bits)          // msg_size:StorageUsed
           && block::tlb::t_Grams.store_long(cb, bp.fwd_fees_collected)  // msg_fees:Grams
           && block::tlb::t_Grams.store_long(cb, bp.fwd_fees);           // fwd_fees:Grams
  }
}

/**
 * Estimates the block storage profile increment if the transaction is added to the block.
 *
 * @param store_stat The current storage statistics of the block.
 * @param usage_tree The usage tree of the block.
 *
 * @returns The estimated block storage profile increment.
 *          Returns Error if the transaction is not serialized or if its new state is not computed.
 */
td::Result<vm::NewCellStorageStat::Stat> Transaction::estimate_block_storage_profile_incr(
    const vm::NewCellStorageStat& store_stat, const vm::CellUsageTree* usage_tree) const {
  if (root.is_null()) {
    return td::Status::Error("Cannot estimate the size profile of a transaction before it is serialized");
  }
  if (new_total_state.is_null()) {
    return td::Status::Error("Cannot estimate the size profile of a transaction before its new state is computed");
  }
  return store_stat.tentative_add_proof(new_total_state, usage_tree) + store_stat.tentative_add_cell(root);
}

/**
 * Updates the limits status of a block.
 *
 * @param blimst The block limit status object to update.
 * @param with_size Flag indicating whether to update the size limits.
 *
 * @returns True if the limits were successfully updated, False otherwise.
 */
bool Transaction::update_limits(block::BlockLimitStatus& blimst, bool with_gas, bool with_size) const {
  if (!(blimst.update_lt(end_lt) && blimst.update_gas(with_gas ? gas_used() : 0))) {
    return false;
  }
  if (with_size) {
    if (!(blimst.add_proof(new_total_state) && blimst.add_cell(root) && blimst.add_transaction() &&
          blimst.add_account(is_first))) {
      return false;
    }
    if (account.is_masterchain()) {
      if (was_frozen || was_deleted) {
        blimst.public_library_diff += get_public_libraries_count(account.orig_library);
      } else {
        blimst.public_library_diff += get_public_libraries_diff_count(account.orig_library, new_library);
      }
    }
  }
  return true;
}

/*
 * 
 *  COMMIT TRANSACTION
 * 
 */

/**
 * Commits a transaction for a given account.
 *
 * @param acc The account to commit the transaction for.
 *
 * @returns A reference to the root cell of the serialized transaction.
 */
Ref<vm::Cell> Transaction::commit(Account& acc) {
  CHECK(account.last_trans_end_lt_ <= start_lt && start_lt < end_lt);
  CHECK(root.not_null());
  CHECK(new_total_state.not_null());
  CHECK((const void*)&acc == (const void*)&account);
  // export all fields modified by the Transaction into original account
  // NB: this is the only method that modifies account
  if (force_remove_anycast_address) {
    CHECK(acc.forget_addr_rewrite_length());
  } else if (orig_addr_rewrite_set && new_addr_rewrite_length >= 0 && acc.status != Account::acc_active &&
      acc_status == Account::acc_active) {
    LOG(DEBUG) << "setting address rewriting info for newly-activated account " << acc.addr.to_hex()
               << " with addr_rewrite_length=" << new_addr_rewrite_length
               << ", orig_addr_rewrite=" << orig_addr_rewrite.bits().to_hex(new_addr_rewrite_length);
    CHECK(acc.init_rewrite_addr(new_addr_rewrite_length, orig_addr_rewrite.bits()));
  }
  acc.status = (acc_status == Account::acc_deleted ? Account::acc_nonexist : acc_status);
  acc.last_trans_lt_ = start_lt;
  acc.last_trans_end_lt_ = end_lt;
  acc.last_trans_hash_ = root->get_hash().bits();
  acc.last_paid = last_paid;
  acc.storage_used = new_storage_used;
  if (new_account_storage_stat) {
    if (acc.account_storage_stat) {
      acc.account_storage_stat.value().apply_child_stat(std::move(new_account_storage_stat.value()));
    } else {
      acc.account_storage_stat = std::move(new_account_storage_stat);
    }
  }
  acc.storage_dict_hash = new_storage_dict_hash;
  acc.storage = new_storage;
  acc.balance = std::move(balance);
  acc.due_payment = std::move(due_payment);
  acc.total_state = std::move(new_total_state);
  acc.inner_state = std::move(new_inner_state);
  if (was_frozen) {
    acc.state_hash = frozen_hash;
  }
  acc.my_addr = std::move(my_addr);
  // acc.my_addr_exact = std::move(my_addr_exact);
  acc.code = std::move(new_code);
  acc.data = std::move(new_data);
  acc.library = std::move(new_library);
  if (acc.status == Account::acc_active) {
    acc.tick = new_tick;
    acc.tock = new_tock;
    acc.fixed_prefix_length = new_fixed_prefix_length;
  } else {
    CHECK(acc.deactivate());
  }
  end_lt = 0;
  acc.push_transaction(root, start_lt);
  return root;
}

/**
 * Extracts the output message at the specified index from the transaction.
 *
 * @param i The index of the output message to extract.
 *
 * @returns A pair of the logical time and the extracted output message.
 */
LtCellRef Transaction::extract_out_msg(unsigned i) {
  return {start_lt + i + 1, std::move(out_msgs.at(i))};
}

/**
 * Extracts the output message at index i from the transaction.
 *
 * @param i The index of the output message to extract.
 *
 * @returns A triple of the logical time, the extracted output message and the transaction root.
 */
NewOutMsg Transaction::extract_out_msg_ext(unsigned i) {
  return {start_lt + i + 1, std::move(out_msgs.at(i)), root, i};
}

/**
 * Extracts the outgoing messages from the transaction and adds them to the given list.
 *
 * @param list The list to which the outgoing messages will be added.
 */
void Transaction::extract_out_msgs(std::vector<LtCellRef>& list) {
  for (unsigned i = 0; i < out_msgs.size(); i++) {
    list.emplace_back(start_lt + i + 1, std::move(out_msgs[i]));
  }
}
}  // namespace transaction

/**
 * Adds a transaction to the account's transaction list.
 *
 * @param trans_root The root of the transaction cell.
 * @param trans_lt The logical time of the transaction.
 */
void Account::push_transaction(Ref<vm::Cell> trans_root, ton::LogicalTime trans_lt) {
  transactions.emplace_back(trans_lt, std::move(trans_root));
}

/**
 * Serializes an account block for the account using AccountBlock TLB-scheme.
 *
 * @param cb The CellBuilder used to store the serialized data.
 *
 * @returns True if the account block was successfully created, false otherwise.
 */
bool Account::create_account_block(vm::CellBuilder& cb) {
  if (transactions.empty()) {
    return false;
  }
  if (!(cb.store_long_bool(5, 4)         // acc_trans#5
        && cb.store_bits_bool(addr))) {  // account_addr:bits256
    return false;
  }
  vm::AugmentedDictionary dict{64, block::tlb::aug_AccountTransactions};
  for (auto& z : transactions) {
    if (!dict.set_ref(td::BitArray<64>{(long long)z.first}, z.second, vm::Dictionary::SetMode::Add)) {
      LOG(ERROR) << "error creating the list of transactions for account " << addr.to_hex()
                 << " : cannot add transaction with lt=" << z.first;
      return false;
    }
  }
  Ref<vm::Cell> dict_root = std::move(dict).extract_root_cell();
  // transactions:(HashmapAug 64 ^Transaction Grams)
  if (dict_root.is_null() || !cb.append_cellslice_bool(vm::load_cell_slice(std::move(dict_root)))) {
    return false;
  }
  vm::CellBuilder cb2;
  return cb2.store_long_bool(0x72, 8)                                      // update_hashes#72
         && cb2.store_bits_bool(orig_total_state->get_hash().bits(), 256)  // old_hash:bits256
         && cb2.store_bits_bool(total_state->get_hash().bits(), 256)       // new_hash:bits256
         && cb.store_ref_bool(cb2.finalize());                             // state_update:^(HASH_UPDATE Account)
}

/**
 * Checks if the libraries stored in the account object have changed.
 *
 * @returns True if the libraries have changed, False otherwise.
 */
bool Account::libraries_changed() const {
  bool s = orig_library.not_null();
  bool t = library.not_null();
  if (s & t) {
    return orig_library->get_hash() != library->get_hash();
  } else {
    return s != t;
  }
}

/**
 * Fetches and initializes various configuration parameters from masterchain config for transaction processing.
 *
 * @param config The masterchain configuration.
 * @param old_mparams Pointer to store a dictionary of mandatory parameters (ConfigParam 9).
 * @param storage_prices Pointer to store the storage prices.
 * @param storage_phase_cfg Pointer to store the storage phase configuration.
 * @param rand_seed Pointer to the random seed. Generates a new seed if the value is `td::Bits256::zero()`.
 * @param compute_phase_cfg Pointer to store the compute phase configuration.
 * @param action_phase_cfg Pointer to store the action phase configuration.
 * @param serialize_cfg Pointer to store the serialize phase configuration.
 * @param masterchain_create_fee Pointer to store the masterchain create fee.
 * @param basechain_create_fee Pointer to store the basechain create fee.
 * @param wc The workchain ID.
 * @param now The current Unix time.
 */
td::Status FetchConfigParams::fetch_config_params(
    const block::ConfigInfo& config, Ref<vm::Cell>* old_mparams, std::vector<block::StoragePrices>* storage_prices,
    StoragePhaseConfig* storage_phase_cfg, td::BitArray<256>* rand_seed, ComputePhaseConfig* compute_phase_cfg,
    ActionPhaseConfig* action_phase_cfg, SerializeConfig* serialize_cfg, td::RefInt256* masterchain_create_fee,
    td::RefInt256* basechain_create_fee, ton::WorkchainId wc, ton::UnixTime now) {
  auto prev_blocks_info = config.get_prev_blocks_info();
  if (prev_blocks_info.is_error()) {
    return prev_blocks_info.move_as_error_prefix(
        td::Status::Error(-668, "cannot fetch prev blocks info from masterchain configuration: "));
  }
  return fetch_config_params(config, prev_blocks_info.move_as_ok(), old_mparams, storage_prices, storage_phase_cfg,
                             rand_seed, compute_phase_cfg, action_phase_cfg, serialize_cfg, masterchain_create_fee,
                             basechain_create_fee, wc, now);
}

/**
 * Fetches and initializes various configuration parameters from masterchain config for transaction processing.
 *
 * @param config The masterchain configuration.
 * @param prev_blocks_info The tuple with information about previous blocks.
 * @param old_mparams Pointer to store a dictionary of mandatory parameters (ConfigParam 9).
 * @param storage_prices Pointer to store the storage prices.
 * @param storage_phase_cfg Pointer to store the storage phase configuration.
 * @param rand_seed Pointer to the random seed. Generates a new seed if the value is `td::Bits256::zero()`.
 * @param compute_phase_cfg Pointer to store the compute phase configuration.
 * @param action_phase_cfg Pointer to store the action phase configuration.
 * @param serialize_cfg Pointer to store the serialize phase configuration.
 * @param masterchain_create_fee Pointer to store the masterchain create fee.
 * @param basechain_create_fee Pointer to store the basechain create fee.
 * @param wc The workchain ID.
 * @param now The current Unix time.
 */
td::Status FetchConfigParams::fetch_config_params(
    const block::Config& config, td::Ref<vm::Tuple> prev_blocks_info, Ref<vm::Cell>* old_mparams,
    std::vector<block::StoragePrices>* storage_prices, StoragePhaseConfig* storage_phase_cfg,
    td::BitArray<256>* rand_seed, ComputePhaseConfig* compute_phase_cfg, ActionPhaseConfig* action_phase_cfg,
    SerializeConfig* serialize_cfg, td::RefInt256* masterchain_create_fee, td::RefInt256* basechain_create_fee,
    ton::WorkchainId wc, ton::UnixTime now) {
  *old_mparams = config.get_config_param(9);
  {
    auto res = config.get_storage_prices();
    if (res.is_error()) {
      return res.move_as_error();
    }
    *storage_prices = res.move_as_ok();
  }
  if (rand_seed->is_zero()) {
    // generate rand seed
    prng::rand_gen().strong_rand_bytes(rand_seed->data(), 32);
    LOG(DEBUG) << "block random seed set to " << rand_seed->to_hex();
  }
  TRY_RESULT(size_limits, config.get_size_limits_config());
  {
    // compute compute_phase_cfg / storage_phase_cfg
    auto cell = config.get_config_param(wc == ton::masterchainId ? 20 : 21);
    if (cell.is_null()) {
      return td::Status::Error(-668, "cannot fetch current gas prices and limits from masterchain configuration");
    }
    if (!compute_phase_cfg->parse_GasLimitsPrices(std::move(cell), storage_phase_cfg->freeze_due_limit,
                                                  storage_phase_cfg->delete_due_limit)) {
      return td::Status::Error(-668, "cannot unpack current gas prices and limits from masterchain configuration");
    }
    TRY_RESULT_PREFIX(mc_gas_prices, config.get_gas_limits_prices(true),
                      "cannot unpack masterchain gas prices and limits: ");
    compute_phase_cfg->mc_gas_prices = std::move(mc_gas_prices);
    compute_phase_cfg->special_gas_full = config.get_global_version() >= 5;
    storage_phase_cfg->enable_due_payment = config.get_global_version() >= 4;
    storage_phase_cfg->global_version = config.get_global_version();
    compute_phase_cfg->block_rand_seed = *rand_seed;
    compute_phase_cfg->max_vm_data_depth = size_limits.max_vm_data_depth;
    compute_phase_cfg->global_config = config.get_root_cell();
    compute_phase_cfg->global_version = config.get_global_version();
    if (compute_phase_cfg->global_version >= 4) {
      compute_phase_cfg->prev_blocks_info = std::move(prev_blocks_info);
    }
    if (compute_phase_cfg->global_version >= 6) {
      compute_phase_cfg->unpacked_config_tuple = config.get_unpacked_config_tuple(now);
    }
    compute_phase_cfg->suspended_addresses = config.get_suspended_addresses(now);
    compute_phase_cfg->size_limits = size_limits;
    compute_phase_cfg->precompiled_contracts = config.get_precompiled_contracts_config();
    compute_phase_cfg->allow_external_unfreeze = compute_phase_cfg->global_version >= 8;
    compute_phase_cfg->disable_anycast = config.get_global_version() >= 10;
  }
  {
    // compute action_phase_cfg
    block::gen::MsgForwardPrices::Record rec;
    auto cell = config.get_config_param(24);
    if (cell.is_null() || !tlb::unpack_cell(std::move(cell), rec)) {
      return td::Status::Error(-668, "cannot fetch masterchain message transfer prices from masterchain configuration");
    }
    action_phase_cfg->fwd_mc =
        block::MsgPrices{rec.lump_price,           rec.bit_price,          rec.cell_price, rec.ihr_price_factor,
                         (unsigned)rec.first_frac, (unsigned)rec.next_frac};
    cell = config.get_config_param(25);
    if (cell.is_null() || !tlb::unpack_cell(std::move(cell), rec)) {
      return td::Status::Error(-668, "cannot fetch standard message transfer prices from masterchain configuration");
    }
    action_phase_cfg->fwd_std =
        block::MsgPrices{rec.lump_price,           rec.bit_price,          rec.cell_price, rec.ihr_price_factor,
                         (unsigned)rec.first_frac, (unsigned)rec.next_frac};
    action_phase_cfg->workchains = &config.get_workchain_list();
    action_phase_cfg->bounce_msg_body = (config.has_capability(ton::capBounceMsgBody) ? 256 : 0);
    action_phase_cfg->size_limits = size_limits;
    action_phase_cfg->action_fine_enabled = config.get_global_version() >= 4;
    action_phase_cfg->bounce_on_fail_enabled = config.get_global_version() >= 4;
    action_phase_cfg->message_skip_enabled = config.get_global_version() >= 8;
    action_phase_cfg->disable_custom_fess = config.get_global_version() >= 8;
    action_phase_cfg->reserve_extra_enabled = config.get_global_version() >= 9;
    action_phase_cfg->mc_blackhole_addr = config.get_burning_config().blackhole_addr;
    action_phase_cfg->extra_currency_v2 = config.get_global_version() >= 10;
    action_phase_cfg->disable_anycast = config.get_global_version() >= 10;
  }
  {
    serialize_cfg->extra_currency_v2 = config.get_global_version() >= 10;
    serialize_cfg->disable_anycast = config.get_global_version() >= 10;
    serialize_cfg->store_storage_dict_hash = config.get_global_version() >= 11;
  }
  {
    // fetch block_grams_created
    auto cell = config.get_config_param(14);
    if (cell.is_null()) {
      *basechain_create_fee = *masterchain_create_fee = td::zero_refint();
    } else {
      block::gen::BlockCreateFees::Record create_fees;
      if (!(tlb::unpack_cell(cell, create_fees) &&
            block::tlb::t_Grams.as_integer_to(create_fees.masterchain_block_fee, *masterchain_create_fee) &&
            block::tlb::t_Grams.as_integer_to(create_fees.basechain_block_fee, *basechain_create_fee))) {
        return td::Status::Error(-668, "cannot unpack BlockCreateFees from configuration parameter #14");
      }
    }
  }
  return td::Status::OK();
}

}  // namespace block
