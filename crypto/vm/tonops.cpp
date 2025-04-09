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
#include <functional>
#include "vm/tonops.h"
#include "vm/log.h"
#include "vm/opctable.h"
#include "vm/stack.hpp"
#include "vm/excno.hpp"
#include "vm/vm.h"
#include "vm/dict.h"
#include "vm/boc.h"
#include "Ed25519.h"
#include "vm/Hasher.h"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "crypto/ellcurve/secp256k1.h"
#include "crypto/ellcurve/p256.h"

#include "openssl/digest.hpp"
#include <sodium.h>
#include "bls.h"
#include "mc-config.h"

namespace vm {

namespace {

bool debug(const char* str) TD_UNUSED;
bool debug(const char* str) {
  std::cerr << str;
  return true;
}

bool debug(int x) TD_UNUSED;
bool debug(int x) {
  if (x < 100) {
    std::cerr << '[' << (char)(64 + x) << ']';
  } else {
    std::cerr << '[' << (char)(64 + x / 100) << x % 100 << ']';
  }
  return true;
}
}  // namespace

#define DBG_START int dbg = 0;
#define DBG debug(++dbg)&&
#define DEB_START DBG_START
#define DEB DBG

static constexpr int randseed_idx = 6;
static constexpr int inmsgparams_idx = 17;

int exec_set_gas_generic(VmState* st, long long new_gas_limit) {
  if (new_gas_limit < st->gas_consumed()) {
    throw VmNoGas{};
  }
  st->change_gas_limit(new_gas_limit);
  if (st->get_stop_on_accept_message()) {
    VM_LOG(st) << "External message is accepted, stopping TVM";
    return st->jump(td::Ref<QuitCont>{true, 0});
  }
  return 0;
}

int exec_accept(VmState* st) {
  VM_LOG(st) << "execute ACCEPT";
  return exec_set_gas_generic(st, GasLimits::infty);
}

int exec_set_gas_limit(VmState* st) {
  VM_LOG(st) << "execute SETGASLIMIT";
  td::RefInt256 x = st->get_stack().pop_int_finite();
  long long gas = 0;
  if (x->sgn() > 0) {
    gas = x->unsigned_fits_bits(63) ? x->to_long() : GasLimits::infty;
  }
  return exec_set_gas_generic(st, gas);
}

int exec_gas_consumed(VmState* st) {
  VM_LOG(st) << "execute GASCONSUMED";
  st->get_stack().push_smallint(st->gas_consumed());
  return 0;
}

int exec_commit(VmState* st) {
  VM_LOG(st) << "execute COMMIT";
  st->force_commit();
  return 0;
}

void register_basic_gas_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xf800, 16, "ACCEPT", exec_accept))
      .insert(OpcodeInstr::mksimple(0xf801, 16, "SETGASLIMIT", exec_set_gas_limit))
      .insert(OpcodeInstr::mksimple(0xf807, 16, "GASCONSUMED", exec_gas_consumed)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf80f, 16, "COMMIT", exec_commit));
}

void register_ton_gas_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
}

static const StackEntry& get_param(VmState* st, unsigned idx) {
  auto tuple = st->get_c7();
  auto t1 = tuple_index(tuple, 0).as_tuple_range(255);
  if (t1.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a tuple"};
  }
  return tuple_index(t1, idx);
}

// ConfigParams: 18 (only one entry), 19, 20, 21, 24, 25, 43
static td::Ref<Tuple> get_unpacked_config_tuple(VmState* st) {
  auto tuple = st->get_c7();
  auto t1 = tuple_index(tuple, 0).as_tuple_range(255);
  if (t1.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a tuple"};
  }
  auto t2 = tuple_index(t1, 14).as_tuple_range(255);
  if (t2.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a tuple"};
  }
  return t2;
}

int exec_get_param(VmState* st, unsigned idx, const char* name) {
  if (name) {
    VM_LOG(st) << "execute " << name;
  }
  Stack& stack = st->get_stack();
  stack.push(get_param(st, idx));
  return 0;
}

int exec_get_var_param(VmState* st, unsigned idx) {
  idx &= 15;
  VM_LOG(st) << "execute GETPARAM " << idx;
  return exec_get_param(st, idx, nullptr);
}

int exec_get_var_param_long(VmState* st, unsigned idx) {
  idx &= 255;
  VM_LOG(st) << "execute GETPARAMLONG " << idx;
  return exec_get_param(st, idx, nullptr);
}

int exec_get_in_msg_param(VmState* st, unsigned idx, const char* name) {
  if (name) {
    VM_LOG(st) << "execute " << name;
  }
  Ref<Tuple> t = get_param(st, inmsgparams_idx).as_tuple();
  st->get_stack().push(tuple_index(t, idx));
  return 0;
}

int exec_get_var_in_msg_param(VmState* st, unsigned idx) {
  idx &= 15;
  VM_LOG(st) << "execute INMSGPARAM " << idx;
  return exec_get_in_msg_param(st, idx, nullptr);
}

int exec_get_config_dict(VmState* st) {
  exec_get_param(st, 9, "CONFIGDICT");
  st->get_stack().push_smallint(32);
  return 0;
}

int exec_get_config_param(VmState* st, bool opt) {
  VM_LOG(st) << "execute CONFIG" << (opt ? "OPTPARAM" : "PARAM");
  Stack& stack = st->get_stack();
  auto idx = stack.pop_int();
  exec_get_param(st, 9, nullptr);
  Dictionary dict{stack.pop_maybe_cell(), 32};
  td::BitArray<32> key;
  Ref<vm::Cell> value;
  if (idx->export_bits(key.bits(), key.size(), true)) {
    value = dict.lookup_ref(key);
  }
  if (opt) {
    stack.push_maybe_cell(std::move(value));
  } else if (value.not_null()) {
    stack.push_cell(std::move(value));
    stack.push_bool(true);
  } else {
    stack.push_bool(false);
  }
  return 0;
}

int exec_get_global_common(VmState* st, unsigned n) {
  st->get_stack().push(tuple_extend_index(st->get_c7(), n));
  return 0;
}

int exec_get_global(VmState* st, unsigned args) {
  args &= 31;
  VM_LOG(st) << "execute GETGLOB " << args;
  return exec_get_global_common(st, args);
}

int exec_get_global_var(VmState* st) {
  VM_LOG(st) << "execute GETGLOBVAR";
  st->check_underflow(1);
  unsigned args = st->get_stack().pop_smallint_range(254);
  return exec_get_global_common(st, args);
}

int exec_set_global_common(VmState* st, unsigned idx) {
  Stack& stack = st->get_stack();
  auto x = stack.pop();
  auto tuple = st->get_c7();
  if (idx >= 255) {
    throw VmError{Excno::range_chk, "tuple index out of range"};
  }
  static auto empty_tuple = Ref<Tuple>{true};
  st->set_c7(empty_tuple);  // optimization; use only if no exception can be thrown until true set_c7()
  auto tpay = tuple_extend_set_index(tuple, idx, std::move(x));
  if (tpay > 0) {
    st->consume_tuple_gas(tpay);
  }
  st->set_c7(std::move(tuple));
  return 0;
}

int exec_set_global(VmState* st, unsigned args) {
  args &= 31;
  VM_LOG(st) << "execute SETGLOB " << args;
  st->check_underflow(1);
  return exec_set_global_common(st, args);
}

int exec_set_global_var(VmState* st) {
  VM_LOG(st) << "execute SETGLOBVAR";
  st->check_underflow(2);
  unsigned args = st->get_stack().pop_smallint_range(254);
  return exec_set_global_common(st, args);
}

int exec_get_prev_blocks_info(VmState* st, unsigned idx, const char* name) {
  idx &= 3;
  VM_LOG(st) << "execute " << name;
  Stack& stack = st->get_stack();
  auto tuple = st->get_c7();
  auto t1 = tuple_index(tuple, 0).as_tuple_range(255);
  if (t1.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a tuple"};
  }
  auto t2 = tuple_index(t1, 13).as_tuple_range(255);
  if (t2.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a tuple"};
  }
  stack.push(tuple_index(t2, idx));
  return 0;
}

int exec_get_global_id(VmState* st) {
  VM_LOG(st) << "execute GLOBALID";
  if (st->get_global_version() >= 6) {
    Ref<CellSlice> cs = tuple_index(get_unpacked_config_tuple(st), 1).as_slice();
    if (cs.is_null()) {
      throw VmError{Excno::type_chk, "intermediate value is not a slice"};
    }
    if (cs->size() < 32) {
      throw VmError{Excno::cell_und, "invalid global-id config"};
    }
    st->get_stack().push_smallint(cs->prefetch_long(32));
  } else {
    Ref<Cell> config = get_param(st, 19).as_cell();
    if (config.is_null()) {
      throw VmError{Excno::type_chk, "intermediate value is not a cell"};
    }
    Dictionary config_dict{std::move(config), 32};
    Ref<Cell> cell = config_dict.lookup_ref(td::BitArray<32>{19});
    if (cell.is_null()) {
      throw VmError{Excno::unknown, "invalid global-id config"};
    }
    CellSlice cs = load_cell_slice(cell);
    if (cs.size() < 32) {
      throw VmError{Excno::unknown, "invalid global-id config"};
    }
    st->get_stack().push_smallint(cs.fetch_long(32));
  }
  return 0;
}

int exec_get_gas_fee(VmState* st) {
  VM_LOG(st) << "execute GETGASFEE";
  Stack& stack = st->get_stack();
  stack.check_underflow(st->get_global_version() >= 9 ? 2 : 0);
  bool is_masterchain = stack.pop_bool();
  td::uint64 gas = stack.pop_long_range(std::numeric_limits<td::int64>::max(), 0);
  block::GasLimitsPrices prices = util::get_gas_prices(get_unpacked_config_tuple(st), is_masterchain);
  stack.push_int(prices.compute_gas_price(gas));
  return 0;
}

int exec_get_storage_fee(VmState* st) {
  VM_LOG(st) << "execute GETSTORAGEFEE";
  Stack& stack = st->get_stack();
  stack.check_underflow(st->get_global_version() >= 9 ? 4 : 0);
  bool is_masterchain = stack.pop_bool();
  td::int64 delta = stack.pop_long_range(std::numeric_limits<td::int64>::max(), 0);
  td::uint64 bits = stack.pop_long_range(std::numeric_limits<td::int64>::max(), 0);
  td::uint64 cells = stack.pop_long_range(std::numeric_limits<td::int64>::max(), 0);
  td::optional<block::StoragePrices> maybe_prices =
      util::get_storage_prices(get_unpacked_config_tuple(st));
  stack.push_int(util::calculate_storage_fee(maybe_prices, is_masterchain, delta, bits, cells));
  return 0;
}

int exec_get_forward_fee(VmState* st) {
  VM_LOG(st) << "execute GETFORWARDFEE";
  Stack& stack = st->get_stack();
  stack.check_underflow(st->get_global_version() >= 9 ? 3 : 0);
  bool is_masterchain = stack.pop_bool();
  td::uint64 bits = stack.pop_long_range(std::numeric_limits<td::int64>::max(), 0);
  td::uint64 cells = stack.pop_long_range(std::numeric_limits<td::int64>::max(), 0);
  block::MsgPrices prices = util::get_msg_prices(get_unpacked_config_tuple(st), is_masterchain);
  stack.push_int(prices.compute_fwd_fees256(cells, bits));
  return 0;
}

int exec_get_precompiled_gas(VmState* st) {
  VM_LOG(st) << "execute GETPRECOMPILEDGAS";
  Stack& stack = st->get_stack();
  stack.push(get_param(st, 16));
  return 0;
}

int exec_get_original_fwd_fee(VmState* st) {
  VM_LOG(st) << "execute GETORIGINALFWDFEE";
  Stack& stack = st->get_stack();
  stack.check_underflow(st->get_global_version() >= 9 ? 2 : 0);
  bool is_masterchain = stack.pop_bool();
  td::RefInt256 fwd_fee = stack.pop_int_finite();
  if (fwd_fee->sgn() < 0) {
    throw VmError{Excno::range_chk, "fwd_fee is negative"};
  }
  block::MsgPrices prices = util::get_msg_prices(get_unpacked_config_tuple(st), is_masterchain);
  stack.push_int(td::muldiv(fwd_fee, td::make_refint(1 << 16), td::make_refint((1 << 16) - prices.first_frac)));
  return 0;
}

int exec_get_gas_fee_simple(VmState* st) {
  VM_LOG(st) << "execute GETGASFEESIMPLE";
  Stack& stack = st->get_stack();
  stack.check_underflow(st->get_global_version() >= 9 ? 2 : 0);
  bool is_masterchain = stack.pop_bool();
  td::uint64 gas = stack.pop_long_range(std::numeric_limits<td::int64>::max(), 0);
  block::GasLimitsPrices prices = util::get_gas_prices(get_unpacked_config_tuple(st), is_masterchain);
  stack.push_int(td::rshift(td::make_refint(prices.gas_price) * gas, 16, 1));
  return 0;
}

int exec_get_forward_fee_simple(VmState* st) {
  VM_LOG(st) << "execute GETFORWARDFEESIMPLE";
  Stack& stack = st->get_stack();
  stack.check_underflow(st->get_global_version() >= 9 ? 3 : 0);
  bool is_masterchain = stack.pop_bool();
  td::uint64 bits = stack.pop_long_range(std::numeric_limits<td::int64>::max(), 0);
  td::uint64 cells = stack.pop_long_range(std::numeric_limits<td::int64>::max(), 0);
  block::MsgPrices prices = util::get_msg_prices(get_unpacked_config_tuple(st), is_masterchain);
  stack.push_int(td::rshift(td::make_refint(prices.bit_price) * bits + td::make_refint(prices.cell_price) * cells, 16,
                            1));  // divide by 2^16 with ceil rounding
  return 0;
}

int exec_get_extra_currency_balance(VmState* st) {
  VM_LOG(st) << "execute GETEXTRABALANCE";
  Stack& stack = st->get_stack();
  auto id = (td::uint32)stack.pop_long_range((1LL << 32) - 1);

  auto tuple = st->get_c7();
  tuple = tuple_index(tuple, 0).as_tuple_range(255);
  if (tuple.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a tuple"};
  }
  tuple = tuple_index(tuple, 7).as_tuple_range(255);  // Balance
  if (tuple.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a tuple"};
  }
  auto dict_root = tuple_index(tuple, 1);
  if (!dict_root.is_cell() && !dict_root.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not cell or null"};
  }

  class LocalVmState : public VmStateInterface {
   public:
    explicit LocalVmState(VmState* st) : st_(st) {
    }
    ~LocalVmState() override = default;

    Ref<Cell> load_library(td::ConstBitPtr hash) override {
      return st_->load_library(hash);
    }
    void register_cell_load(const CellHash& cell_hash) override {
      auto new_cell = st_->register_cell_load_free(cell_hash);
      consume_gas(new_cell ? VmState::cell_load_gas_price : VmState::cell_reload_gas_price);
    }
    void register_cell_create() override {
      // Not expected in this operation
    }
    int get_global_version() const override {
      return st_->get_global_version();
    }

   private:
    VmState* st_;
    long long remaining = VmState::get_extra_balance_cheap_max_gas_price;

    void consume_gas(long long gas) {
      long long consumed = std::min(gas, remaining);
      st_->consume_gas(consumed);
      remaining -= consumed;
      if (remaining == 0) {
        st_->consume_free_gas(gas - consumed);
      }
    }
  };
  bool cheap = st->register_get_extra_balance_call();
  LocalVmState local_vm_state{st};
  VmStateInterface::Guard guard{cheap ? (VmStateInterface*)&local_vm_state : st};

  Dictionary dict{dict_root.as_cell(), 32};
  Ref<CellSlice> cs = dict.lookup(td::BitArray<32>(id));
  if (cs.is_null()) {
    stack.push_smallint(0);
  } else {
    td::RefInt256 x;
    util::load_var_integer_q(cs.write(), x, /* len_bits = */ 5, /* sgnd = */ false, /* quiet = */ false);
    stack.push_int(std::move(x));
  }

  return 0;
}

void register_ton_config_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mkfixedrange(0xf820, 0xf823, 16, 4, instr::dump_1c("GETPARAM "), exec_get_var_param))
      .insert(OpcodeInstr::mksimple(0xf823, 16, "NOW", std::bind(exec_get_param, _1, 3, "NOW")))
      .insert(OpcodeInstr::mksimple(0xf824, 16, "BLOCKLT", std::bind(exec_get_param, _1, 4, "BLOCKLT")))
      .insert(OpcodeInstr::mksimple(0xf825, 16, "LTIME", std::bind(exec_get_param, _1, 5, "LTIME")))
      .insert(OpcodeInstr::mksimple(0xf826, 16, "RANDSEED", std::bind(exec_get_param, _1, 6, "RANDSEED")))
      .insert(OpcodeInstr::mksimple(0xf827, 16, "BALANCE", std::bind(exec_get_param, _1, 7, "BALANCE")))
      .insert(OpcodeInstr::mksimple(0xf828, 16, "MYADDR", std::bind(exec_get_param, _1, 8, "MYADDR")))
      .insert(OpcodeInstr::mksimple(0xf829, 16, "CONFIGROOT", std::bind(exec_get_param, _1, 9, "CONFIGROOT")))
      .insert(OpcodeInstr::mksimple(0xf82a, 16, "MYCODE", std::bind(exec_get_param, _1, 10, "MYCODE")))
      .insert(OpcodeInstr::mksimple(0xf82b, 16, "INCOMINGVALUE", std::bind(exec_get_param, _1, 11, "INCOMINGVALUE")))
      .insert(OpcodeInstr::mksimple(0xf82c, 16, "STORAGEFEES", std::bind(exec_get_param, _1, 12, "STORAGEFEES")))
      .insert(OpcodeInstr::mksimple(0xf82d, 16, "PREVBLOCKSINFOTUPLE", std::bind(exec_get_param, _1, 13, "PREVBLOCKSINFOTUPLE")))
      .insert(OpcodeInstr::mksimple(0xf82e, 16, "UNPACKEDCONFIGTUPLE", std::bind(exec_get_param, _1, 14, "UNPACKEDCONFIGTUPLE")))
      .insert(OpcodeInstr::mksimple(0xf82f, 16, "DUEPAYMENT", std::bind(exec_get_param, _1, 15, "DUEPAYMENT")))
      .insert(OpcodeInstr::mksimple(0xf830, 16, "CONFIGDICT", exec_get_config_dict))
      .insert(OpcodeInstr::mksimple(0xf832, 16, "CONFIGPARAM", std::bind(exec_get_config_param, _1, false)))
      .insert(OpcodeInstr::mksimple(0xf833, 16, "CONFIGOPTPARAM", std::bind(exec_get_config_param, _1, true)))
      .insert(OpcodeInstr::mksimple(0xf83400, 24, "PREVMCBLOCKS", std::bind(exec_get_prev_blocks_info, _1, 0, "PREVMCBLOCKS"))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf83401, 24, "PREVKEYBLOCK", std::bind(exec_get_prev_blocks_info, _1, 1, "PREVKEYBLOCK"))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf83402, 24, "PREVMCBLOCKS_100", std::bind(exec_get_prev_blocks_info, _1, 2, "PREVMCBLOCKS_100"))->require_version(9))
      .insert(OpcodeInstr::mksimple(0xf835, 16, "GLOBALID", exec_get_global_id)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf836, 16, "GETGASFEE", exec_get_gas_fee)->require_version(6))
      .insert(OpcodeInstr::mksimple(0xf837, 16, "GETSTORAGEFEE", exec_get_storage_fee)->require_version(6))
      .insert(OpcodeInstr::mksimple(0xf838, 16, "GETFORWARDFEE", exec_get_forward_fee)->require_version(6))
      .insert(OpcodeInstr::mksimple(0xf839, 16, "GETPRECOMPILEDGAS", exec_get_precompiled_gas)->require_version(6))
      .insert(OpcodeInstr::mksimple(0xf83a, 16, "GETORIGINALFWDFEE", exec_get_original_fwd_fee)->require_version(6))
      .insert(OpcodeInstr::mksimple(0xf83b, 16, "GETGASFEESIMPLE", exec_get_gas_fee_simple)->require_version(6))
      .insert(OpcodeInstr::mksimple(0xf83c, 16, "GETFORWARDFEESIMPLE", exec_get_forward_fee_simple)->require_version(6))
      .insert(OpcodeInstr::mksimple(0xf840, 16, "GETGLOBVAR", exec_get_global_var))
      .insert(OpcodeInstr::mkfixedrange(0xf841, 0xf860, 16, 5, instr::dump_1c_and(31, "GETGLOB "), exec_get_global))
      .insert(OpcodeInstr::mksimple(0xf860, 16, "SETGLOBVAR", exec_set_global_var))
      .insert(OpcodeInstr::mkfixedrange(0xf861, 0xf880, 16, 5, instr::dump_1c_and(31, "SETGLOB "), exec_set_global))
      .insert(OpcodeInstr::mksimple(0xf880, 16, "GETEXTRABALANCE", exec_get_extra_currency_balance)->require_version(10))
      .insert(OpcodeInstr::mkfixedrange(0xf88100, 0xf88111, 24, 8, instr::dump_1c_l_add(0, "GETPARAMLONG "), exec_get_var_param_long)->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf88111, 24, "INMSGPARAMS", std::bind(exec_get_param, _1, 17, "INMSGPARAMS"))->require_version(11))
      .insert(OpcodeInstr::mkfixedrange(0xf88112, 0xf881ff, 24, 8, instr::dump_1c_l_add(0, "GETPARAMLONG "), exec_get_var_param_long)->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf890, 16, "INMSG_BOUNCE", std::bind(exec_get_in_msg_param, _1, 0, "INMSG_BOUNCE"))->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf891, 16, "INMSG_BOUNCED", std::bind(exec_get_in_msg_param, _1, 1, "INMSG_BOUNCED"))->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf892, 16, "INMSG_SRC", std::bind(exec_get_in_msg_param, _1, 2, "INMSG_SRC"))->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf893, 16, "INMSG_FWDFEE", std::bind(exec_get_in_msg_param, _1, 3, "INMSG_FWDFEE"))->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf894, 16, "INMSG_LT", std::bind(exec_get_in_msg_param, _1, 4, "INMSG_LT"))->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf895, 16, "INMSG_UTIME", std::bind(exec_get_in_msg_param, _1, 5, "INMSG_UTIME"))->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf896, 16, "INMSG_ORIGVALUE", std::bind(exec_get_in_msg_param, _1, 6, "INMSG_ORIGVALUE"))->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf897, 16, "INMSG_VALUE", std::bind(exec_get_in_msg_param, _1, 7, "INMSG_VALUE"))->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf898, 16, "INMSG_VALUEEXTRA", std::bind(exec_get_in_msg_param, _1, 8, "INMSG_VALUEEXTRA"))->require_version(11))
      .insert(OpcodeInstr::mksimple(0xf899, 16, "INMSG_STATEINIT", std::bind(exec_get_in_msg_param, _1, 9, "INMSG_STATEINIT"))->require_version(11))
      .insert(OpcodeInstr::mkfixedrange(0xf89a, 0xf8a0, 16, 4, instr::dump_1c("INMSGPARAM "), exec_get_var_in_msg_param)->require_version(11));
}

td::RefInt256 generate_randu256(VmState* st) {
  auto tuple = st->get_c7();
  auto t1 = tuple_index(tuple, 0).as_tuple_range(255);
  if (t1.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a tuple"};
  }
  auto seedv = tuple_index(t1, randseed_idx).as_int();
  if (seedv.is_null()) {
    throw VmError{Excno::type_chk, "random seed is not an integer"};
  }
  unsigned char seed[32];
  if (!seedv->export_bytes(seed, 32, false)) {
    throw VmError{Excno::range_chk, "random seed out of range"};
  }
  unsigned char hash[64];
  digest::hash_str<digest::SHA512>(hash, seed, 32);
  if (!seedv.write().import_bytes(hash, 32, false)) {
    throw VmError{Excno::range_chk, "cannot store new random seed"};
  }
  td::RefInt256 res{true};
  if (!res.write().import_bytes(hash + 32, 32, false)) {
    throw VmError{Excno::range_chk, "cannot store new random number"};
  }
  static auto empty_tuple = Ref<Tuple>{true};
  st->set_c7(empty_tuple);  // optimization; use only if no exception can be thrown until true set_c7()
  tuple.write()[0].clear();
  t1.write().at(randseed_idx) = std::move(seedv);
  st->consume_tuple_gas(t1);
  tuple.write().at(0) = std::move(t1);
  st->consume_tuple_gas(tuple);
  st->set_c7(std::move(tuple));
  return res;
}

int exec_randu256(VmState* st) {
  VM_LOG(st) << "execute RANDU256";
  st->get_stack().push_int(generate_randu256(st));
  return 0;
}

int exec_rand_int(VmState* st) {
  VM_LOG(st) << "execute RAND";
  auto& stack = st->get_stack();
  stack.check_underflow(1);
  auto x = stack.pop_int_finite();
  auto y = generate_randu256(st);
  typename td::BigInt256::DoubleInt tmp{0};
  tmp.add_mul(*x, *y);
  tmp.rshift(256, -1).normalize();
  stack.push_int(td::make_refint(tmp));
  return 0;
}

int exec_set_rand(VmState* st, bool mix) {
  VM_LOG(st) << "execute " << (mix ? "ADDRAND" : "SETRAND");
  auto& stack = st->get_stack();
  stack.check_underflow(1);
  auto x = stack.pop_int_finite();
  if (!x->unsigned_fits_bits(256)) {
    throw VmError{Excno::range_chk, "new random seed out of range"};
  }
  auto tuple = st->get_c7();
  auto t1 = tuple_index(tuple, 0).as_tuple_range(255);
  if (t1.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a tuple"};
  }
  if (mix) {
    auto seedv = tuple_index(t1, randseed_idx).as_int();
    if (seedv.is_null()) {
      throw VmError{Excno::type_chk, "random seed is not an integer"};
    }
    unsigned char buffer[64], hash[32];
    if (!std::move(seedv)->export_bytes(buffer, 32, false)) {
      throw VmError{Excno::range_chk, "random seed out of range"};
    }
    if (!x->export_bytes(buffer + 32, 32, false)) {
      throw VmError{Excno::range_chk, "mixed seed value out of range"};
    }
    digest::hash_str<digest::SHA256>(hash, buffer, 64);
    if (!x.write().import_bytes(hash, 32, false)) {
      throw VmError{Excno::range_chk, "new random seed value out of range"};
    }
  }
  static auto empty_tuple = Ref<Tuple>{true};
  st->set_c7(empty_tuple);  // optimization; use only if no exception can be thrown until true set_c7()
  tuple.write()[0].clear();
  auto tpay = tuple_extend_set_index(t1, randseed_idx, std::move(x));
  if (tpay > 0) {
    st->consume_tuple_gas(tpay);
  }
  tuple.unique_write()[0] = std::move(t1);
  st->consume_tuple_gas(tuple);
  st->set_c7(std::move(tuple));
  return 0;
}

void register_prng_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xf810, 16, "RANDU256", exec_randu256))
      .insert(OpcodeInstr::mksimple(0xf811, 16, "RAND", exec_rand_int))
      .insert(OpcodeInstr::mksimple(0xf814, 16, "SETRAND", std::bind(exec_set_rand, _1, false)))
      .insert(OpcodeInstr::mksimple(0xf815, 16, "ADDRAND", std::bind(exec_set_rand, _1, true)));
}

int exec_compute_hash(VmState* st, int mode) {
  VM_LOG(st) << "execute HASH" << (mode & 1 ? 'S' : 'C') << 'U';
  Stack& stack = st->get_stack();
  std::array<unsigned char, 32> hash;
  if (!(mode & 1)) {
    auto cell = stack.pop_cell();
    hash = cell->get_hash().as_array();
  } else {
    auto cs = stack.pop_cellslice();
    vm::CellBuilder cb;
    CHECK(cb.append_cellslice_bool(std::move(cs)));
    // TODO: use cb.get_hash() instead
    hash = cb.finalize()->get_hash().as_array();
  }
  td::RefInt256 res{true};
  CHECK(res.write().import_bytes(hash.data(), hash.size(), false));
  stack.push_int(std::move(res));
  return 0;
}

int exec_compute_sha256(VmState* st) {
  VM_LOG(st) << "execute SHA256U";
  Stack& stack = st->get_stack();
  auto cs = stack.pop_cellslice();
  if (cs->size() & 7) {
    throw VmError{Excno::cell_und, "Slice does not consist of an integer number of bytes"};
  }
  auto len = (cs->size() >> 3);
  unsigned char data[128], hash[32];
  CHECK(len <= sizeof(data));
  CHECK(cs->prefetch_bytes(data, len));
  digest::hash_str<digest::SHA256>(hash, data, len);
  td::RefInt256 res{true};
  CHECK(res.write().import_bytes(hash, 32, false));
  stack.push_int(std::move(res));
  return 0;
}

int exec_hash_ext(VmState* st, unsigned args) {
  bool rev = (args >> 8) & 1;
  bool append = (args >> 9) & 1;
  int hash_id = args & 255;
  VM_LOG(st) << "execute HASHEXT" << (append ? "A" : "") << (rev ? "R" : "") << " " << (hash_id == 255 ? -1 : hash_id);
  Stack& stack = st->get_stack();
  if (hash_id == 255) {
    stack.check_underflow(st->get_global_version() >= 9 ? 2 : 0);
    hash_id = stack.pop_smallint_range(254);
  }
  int cnt = stack.pop_smallint_range(stack.depth() - 1 - (st->get_global_version() >= 9 ? (int)append : 0));
  Hasher hasher{hash_id};
  size_t total_bits = 0;
  long long gas_consumed = 0;
  for (int i = 0; i < cnt; ++i) {
    td::ConstBitPtr data{nullptr};
    unsigned size;
    int idx = rev ? i : cnt - 1 - i;
    auto slice = stack[idx].as_slice();
    if (slice.not_null()) {
      data = slice->data_bits();
      size = slice->size();
    } else {
      auto builder = stack[idx].as_builder();
      if (builder.not_null()) {
        data = builder->data_bits();
        size = builder->size();
      } else {
        stack.pop_many(cnt);
        throw VmError{Excno::type_chk, "expected slice or builder"};
      }
    }
    total_bits += size;
    long long gas_total = (i + 1) * VmState::hash_ext_entry_gas_price + total_bits / 8 / hasher.bytes_per_gas_unit();
    st->consume_gas(gas_total - gas_consumed);
    gas_consumed = gas_total;
    hasher.append(data, size);
  }
  stack.pop_many(cnt);
  td::BufferSlice hash = hasher.finish();
  if (append) {
    Ref<CellBuilder> builder = stack.pop_builder();
    if (!builder->can_extend_by(hash.size() * 8)) {
      throw VmError{Excno::cell_ov};
    }
    builder.write().store_bytes(hash.as_slice());
    stack.push_builder(std::move(builder));
  } else {
    if (hash.size() <= 32) {
      td::RefInt256 res{true};
      CHECK(res.write().import_bytes((unsigned char*)hash.data(), hash.size(), false));
      stack.push_int(std::move(res));
    } else {
      std::vector<StackEntry> res;
      for (size_t i = 0; i < hash.size(); i += 32) {
        td::RefInt256 x{true};
        CHECK(x.write().import_bytes((unsigned char*)hash.data() + i, std::min<size_t>(hash.size() - i, 32), false));
        res.push_back(std::move(x));
      }
      stack.push_tuple(std::move(res));
    }
  }
  return 0;
}

std::string dump_hash_ext(CellSlice& cs, unsigned args) {
  bool rev = (args >> 8) & 1;
  bool append = (args >> 9) & 1;
  int hash_id = args & 255;
  return PSTRING() << "HASHEXT" << (append ? "A" : "") << (rev ? "R" : "") << " " << (hash_id == 255 ? -1 : hash_id);
}

int exec_ed25519_check_signature(VmState* st, bool from_slice) {
  VM_LOG(st) << "execute CHKSIGN" << (from_slice ? 'S' : 'U');
  Stack& stack = st->get_stack();
  stack.check_underflow(3);
  auto key_int = stack.pop_int();
  auto signature_cs = stack.pop_cellslice();
  unsigned char data[128], key[32], signature[64];
  unsigned data_len;
  if (from_slice) {
    auto cs = stack.pop_cellslice();
    if (cs->size() & 7) {
      throw VmError{Excno::cell_und, "Slice does not consist of an integer number of bytes"};
    }
    data_len = (cs->size() >> 3);
    CHECK(data_len <= sizeof(data));
    CHECK(cs->prefetch_bytes(data, data_len));
  } else {
    auto hash_int = stack.pop_int();
    data_len = 32;
    if (!hash_int->export_bytes(data, data_len, false)) {
      throw VmError{Excno::range_chk, "data hash must fit in an unsigned 256-bit integer"};
    }
  }
  if (!signature_cs->prefetch_bytes(signature, 64)) {
    throw VmError{Excno::cell_und, "Ed25519 signature must contain at least 512 data bits"};
  }
  if (!key_int->export_bytes(key, 32, false)) {
    throw VmError{Excno::range_chk, "Ed25519 public key must fit in an unsigned 256-bit integer"};
  }
  st->register_chksgn_call();
  td::Ed25519::PublicKey pub_key{td::SecureString(td::Slice{key, 32})};
  auto res = pub_key.verify_signature(td::Slice{data, data_len}, td::Slice{signature, 64});
  stack.push_bool(res.is_ok() || st->get_chksig_always_succeed());
  return 0;
}

int exec_ecrecover(VmState* st) {
  VM_LOG(st) << "execute ECRECOVER";
  Stack& stack = st->get_stack();
  stack.check_underflow(4);
  auto s = stack.pop_int();
  auto r = stack.pop_int();
  auto v = (td::uint8)stack.pop_smallint_range(255);
  auto hash = stack.pop_int();

  unsigned char signature[65];
  if (!r->export_bytes(signature, 32, false)) {
    throw VmError{Excno::range_chk, "r must fit in an unsigned 256-bit integer"};
  }
  if (!s->export_bytes(signature + 32, 32, false)) {
    throw VmError{Excno::range_chk, "s must fit in an unsigned 256-bit integer"};
  }
  signature[64] = v;
  unsigned char hash_bytes[32];
  if (!hash->export_bytes(hash_bytes, 32, false)) {
    throw VmError{Excno::range_chk, "data hash must fit in an unsigned 256-bit integer"};
  }
  st->consume_gas(VmState::ecrecover_gas_price);
  unsigned char public_key[65];
  if (td::secp256k1::ecrecover(hash_bytes, signature, public_key)) {
    td::uint8 h = public_key[0];
    td::RefInt256 x1{true}, x2{true};
    CHECK(x1.write().import_bytes(public_key + 1, 32, false));
    CHECK(x2.write().import_bytes(public_key + 33, 32, false));
    stack.push_smallint(h);
    stack.push_int(std::move(x1));
    stack.push_int(std::move(x2));
    stack.push_bool(true);
  } else {
    stack.push_bool(false);
  }
  return 0;
}

int exec_secp256k1_xonly_pubkey_tweak_add(VmState* st) {
  VM_LOG(st) << "execute SECP256K1_XONLY_PUBKEY_TWEAK_ADD";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  auto tweak_int = stack.pop_int();
  auto key_int = stack.pop_int();

  unsigned char key[32], tweak[32];
  if (!key_int->export_bytes(key, 32, false)) {
    throw VmError{Excno::range_chk, "key must fit in an unsigned 256-bit integer"};
  }
  if (!tweak_int->export_bytes(tweak, 32, false)) {
    throw VmError{Excno::range_chk, "tweak must fit in an unsigned 256-bit integer"};
  }
  st->consume_gas(VmState::secp256k1_xonly_pubkey_tweak_add_gas_price);
  unsigned char public_key[65];
  if (td::secp256k1::xonly_pubkey_tweak_add(key, tweak, public_key)) {
    td::uint8 h = public_key[0];
    td::RefInt256 x1{true}, x2{true};
    CHECK(x1.write().import_bytes(public_key + 1, 32, false));
    CHECK(x2.write().import_bytes(public_key + 33, 32, false));
    stack.push_smallint(h);
    stack.push_int(std::move(x1));
    stack.push_int(std::move(x2));
    stack.push_bool(true);
  } else {
    stack.push_bool(false);
  }
  return 0;
}

int exec_p256_chksign(VmState* st, bool from_slice) {
  VM_LOG(st) << "execute P256_CHKSIGN" << (from_slice ? 'S' : 'U');
  Stack& stack = st->get_stack();
  stack.check_underflow(3);
  auto key_cs = stack.pop_cellslice();
  auto signature_cs = stack.pop_cellslice();
  unsigned char data[128], key[33], signature[64];
  unsigned data_len;
  if (from_slice) {
    auto cs = stack.pop_cellslice();
    if (cs->size() & 7) {
      throw VmError{Excno::cell_und, "Slice does not consist of an integer number of bytes"};
    }
    data_len = (cs->size() >> 3);
    CHECK(data_len <= sizeof(data));
    CHECK(cs->prefetch_bytes(data, data_len));
  } else {
    auto hash_int = stack.pop_int();
    data_len = 32;
    if (!hash_int->export_bytes(data, data_len, false)) {
      throw VmError{Excno::range_chk, "data hash must fit in an unsigned 256-bit integer"};
    }
  }
  if (!signature_cs->prefetch_bytes(signature, 64)) {
    throw VmError{Excno::cell_und, "P256 signature must contain at least 512 data bits"};
  }
  if (!key_cs->prefetch_bytes(key, 33)) {
    throw VmError{Excno::cell_und, "P256 public key must contain at least 33 data bytes"};
  }
  st->consume_gas(VmState::p256_chksgn_gas_price);
  auto res = td::p256_check_signature(td::Slice{data, data_len}, td::Slice{key, 33}, td::Slice{signature, 64});
  if (res.is_error()) {
    VM_LOG(st) << "P256_CHKSIGN: " << res.error().message();
  }
  stack.push_bool(res.is_ok() || st->get_chksig_always_succeed());
  return 0;
}

static_assert(crypto_scalarmult_ristretto255_BYTES == 32, "Unexpected value of ristretto255 constant");
static_assert(crypto_scalarmult_ristretto255_SCALARBYTES == 32, "Unexpected value of ristretto255 constant");
static_assert(crypto_core_ristretto255_BYTES == 32, "Unexpected value of ristretto255 constant");
static_assert(crypto_core_ristretto255_HASHBYTES == 64, "Unexpected value of ristretto255 constant");
static_assert(crypto_core_ristretto255_SCALARBYTES == 32, "Unexpected value of ristretto255 constant");
static_assert(crypto_core_ristretto255_NONREDUCEDSCALARBYTES == 64, "Unexpected value of ristretto255 constant");

int exec_ristretto255_from_hash(VmState* st) {
  VM_LOG(st) << "execute RIST255_FROMHASH";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  auto x2 = stack.pop_int();
  auto x1 = stack.pop_int();
  st->consume_gas(VmState::rist255_fromhash_gas_price);
  unsigned char xb[64], rb[32];
  if (!x1->export_bytes(xb, 32, false)) {
    throw VmError{Excno::range_chk, "x1 must fit in an unsigned 256-bit integer"};
  }
  if (!x2->export_bytes(xb + 32, 32, false)) {
    throw VmError{Excno::range_chk, "x2 must fit in an unsigned 256-bit integer"};
  }
  crypto_core_ristretto255_from_hash(rb, xb);
  td::RefInt256 r{true};
  CHECK(r.write().import_bytes(rb, 32, false));
  stack.push_int(std::move(r));
  return 0;
}

int exec_ristretto255_validate(VmState* st, bool quiet) {
  VM_LOG(st) << "execute RIST255_VALIDATE";
  Stack& stack = st->get_stack();
  auto x = stack.pop_int();
  st->consume_gas(VmState::rist255_validate_gas_price);
  unsigned char xb[32];
  if (!x->export_bytes(xb, 32, false) || !crypto_core_ristretto255_is_valid_point(xb)) {
    if (quiet) {
      stack.push_bool(false);
      return 0;
    }
    throw VmError{Excno::range_chk, "x is not a valid encoded element"};
  }
  if (quiet) {
    stack.push_bool(true);
  }
  return 0;
}

int exec_ristretto255_add(VmState* st, bool quiet) {
  VM_LOG(st) << "execute RIST255_ADD";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  auto y = stack.pop_int();
  auto x = stack.pop_int();
  st->consume_gas(VmState::rist255_add_gas_price);
  unsigned char xb[32], yb[32], rb[32];
  if (!x->export_bytes(xb, 32, false) || !y->export_bytes(yb, 32, false) || crypto_core_ristretto255_add(rb, xb, yb)) {
    if (quiet) {
      stack.push_bool(false);
      return 0;
    }
    throw VmError{Excno::range_chk, "x and/or y are not valid encoded elements"};
  }
  td::RefInt256 r{true};
  CHECK(r.write().import_bytes(rb, 32, false));
  stack.push_int(std::move(r));
  if (quiet) {
    stack.push_bool(true);
  }
  return 0;
}

int exec_ristretto255_sub(VmState* st, bool quiet) {
  VM_LOG(st) << "execute RIST255_SUB";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  auto y = stack.pop_int();
  auto x = stack.pop_int();
  st->consume_gas(VmState::rist255_add_gas_price);
  unsigned char xb[32], yb[32], rb[32];
  if (!x->export_bytes(xb, 32, false) || !y->export_bytes(yb, 32, false) || crypto_core_ristretto255_sub(rb, xb, yb)) {
    if (quiet) {
      stack.push_bool(false);
      return 0;
    }
    throw VmError{Excno::range_chk, "x and/or y are not valid encoded elements"};
  }
  td::RefInt256 r{true};
  CHECK(r.write().import_bytes(rb, 32, false));
  stack.push_int(std::move(r));
  if (quiet) {
    stack.push_bool(true);
  }
  return 0;
}

static bool export_bytes_little(const td::RefInt256& n, unsigned char* nb) {
  if (!n->export_bytes(nb, 32, false)) {
    return false;
  }
  std::reverse(nb, nb + 32);
  return true;
}

static td::RefInt256 get_ristretto256_l() {
  static td::RefInt256 l =
      (td::make_refint(1) << 252) + td::dec_string_to_int256(td::Slice("27742317777372353535851937790883648493"));
  return l;
}

int exec_ristretto255_mul(VmState* st, bool quiet) {
  VM_LOG(st) << "execute RIST255_MUL";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  auto n = stack.pop_int() % get_ristretto256_l();
  auto x = stack.pop_int();
  st->consume_gas(VmState::rist255_mul_gas_price);
  if (n->sgn() == 0) {
    stack.push_smallint(0);
    if (quiet) {
      stack.push_bool(true);
    }
    return 0;
  }
  unsigned char xb[32], nb[32], rb[32];
  if (!x->export_bytes(xb, 32, false) || !export_bytes_little(n, nb) || crypto_scalarmult_ristretto255(rb, nb, xb)) {
    if (quiet) {
      stack.push_bool(false);
      return 0;
    }
    throw VmError{Excno::range_chk, "invalid x or n"};
  }
  td::RefInt256 r{true};
  CHECK(r.write().import_bytes(rb, 32, false));
  stack.push_int(std::move(r));
  if (quiet) {
    stack.push_bool(true);
  }
  return 0;
}

int exec_ristretto255_mul_base(VmState* st, bool quiet) {
  VM_LOG(st) << "execute RIST255_MULBASE";
  Stack& stack = st->get_stack();
  auto n = stack.pop_int() % get_ristretto256_l();
  st->consume_gas(VmState::rist255_mulbase_gas_price);
  unsigned char nb[32], rb[32];
  memset(rb, 255, sizeof(rb));
  if (!export_bytes_little(n, nb) || crypto_scalarmult_ristretto255_base(rb, nb)) {
    if (std::all_of(rb, rb + 32, [](unsigned char c) { return c == 255; })) {
      if (quiet) {
        stack.push_bool(false);
        return 0;
      }
      throw VmError{Excno::range_chk, "invalid n"};
    }
  }
  td::RefInt256 r{true};
  CHECK(r.write().import_bytes(rb, 32, false));
  stack.push_int(std::move(r));
  if (quiet) {
    stack.push_bool(true);
  }
  return 0;
}

int exec_ristretto255_push_l(VmState* st) {
  VM_LOG(st) << "execute RIST255_PUSHL";
  Stack& stack = st->get_stack();
  stack.push_int(get_ristretto256_l());
  return 0;
}

static bls::P1 slice_to_bls_p1(const CellSlice& cs) {
  bls::P1 p1;
  if (!cs.prefetch_bytes(p1.as_slice())) {
    throw VmError{Excno::cell_und, PSTRING() << "slice must contain at least " << bls::P1_SIZE << " bytes"};
  }
  return p1;
}

static bls::P2 slice_to_bls_p2(const CellSlice& cs) {
  bls::P2 p2;
  if (!cs.prefetch_bytes(p2.as_slice())) {
    throw VmError{Excno::cell_und, PSTRING() << "slice must contain at least " << bls::P2_SIZE << " bytes"};
  }
  return p2;
}

static bls::FP slice_to_bls_fp(const CellSlice& cs) {
  bls::FP fp;
  if (!cs.prefetch_bytes(fp.as_slice())) {
    throw VmError{Excno::cell_und, PSTRING() << "slice must contain at least " << bls::FP_SIZE << " bytes"};
  }
  return fp;
}

static bls::FP2 slice_to_bls_fp2(const CellSlice& cs) {
  bls::FP2 fp2;
  if (!cs.prefetch_bytes(fp2.as_slice())) {
    throw VmError{Excno::cell_und, PSTRING() << "slice must contain at least " << bls::FP_SIZE * 2 << " bytes"};
  }
  return fp2;
}

static td::BufferSlice slice_to_bls_msg(const CellSlice& cs) {
  if (cs.size() % 8 != 0) {
    throw VmError{Excno::cell_und, "message does not consist of an integer number of bytes"};
  }
  size_t msg_size = cs.size() / 8;
  td::BufferSlice s(msg_size);
  cs.prefetch_bytes((td::uint8*)s.data(), (int)msg_size);
  return s;
}

static Ref<CellSlice> bls_to_slice(td::Slice s) {
  VmStateInterface::Guard guard{nullptr};  // Don't consume gas for finalize and load_cell_slice
  CellBuilder cb;
  return load_cell_slice_ref(cb.store_bytes(s).finalize());
}

static long long bls_calculate_multiexp_gas(int n, long long base, long long coef1, long long coef2) {
  int l = 4;
  while ((1LL << (l + 1)) <= n) {
    ++l;
  }
  return base + n * coef1 + n * coef2 / l;
}

int exec_bls_verify(VmState* st) {
  VM_LOG(st) << "execute BLS_VERIFY";
  Stack& stack = st->get_stack();
  stack.check_underflow(3);
  st->consume_gas(VmState::bls_verify_gas_price);
  bls::P2 sig = slice_to_bls_p2(*stack.pop_cellslice());
  td::BufferSlice msg = slice_to_bls_msg(*stack.pop_cellslice());
  bls::P1 pub = slice_to_bls_p1(*stack.pop_cellslice());
  stack.push_bool(bls::verify(pub, msg, sig));
  return 0;
}

int exec_bls_aggregate(VmState* st) {
  VM_LOG(st) << "execute BLS_AGGREGATE";
  Stack& stack = st->get_stack();
  int n = stack.pop_smallint_range(stack.depth() - 1, 1);
  st->consume_gas(VmState::bls_aggregate_base_gas_price + (long long)n * VmState::bls_aggregate_element_gas_price);
  std::vector<bls::P2> sigs(n);
  for (int i = n - 1; i >= 0; --i) {
    sigs[i] = slice_to_bls_p2(*stack.pop_cellslice());
  }
  bls::P2 aggregated = bls::aggregate(sigs);
  stack.push_cellslice(bls_to_slice(aggregated.as_slice()));
  return 0;
}

int exec_bls_fast_aggregate_verify(VmState* st) {
  VM_LOG(st) << "execute BLS_FASTAGGREGATEVERIFY";
  Stack& stack = st->get_stack();
  stack.check_underflow(3);
  Ref<CellSlice> sig = stack.pop_cellslice();
  Ref<CellSlice> msg = stack.pop_cellslice();
  int n = stack.pop_smallint_range(stack.depth() - 1);
  st->consume_gas(VmState::bls_fast_aggregate_verify_base_gas_price +
                  (long long)n * VmState::bls_fast_aggregate_verify_element_gas_price);
  std::vector<bls::P1> pubs(n);
  for (int i = n - 1; i >= 0; --i) {
    pubs[i] = slice_to_bls_p1(*stack.pop_cellslice());
  }
  stack.push_bool(bls::fast_aggregate_verify(pubs, slice_to_bls_msg(*msg), slice_to_bls_p2(*sig)));
  return 0;
}

int exec_bls_aggregate_verify(VmState* st) {
  VM_LOG(st) << "execute BLS_AGGREGATEVERIFY";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  Ref<CellSlice> sig = stack.pop_cellslice();
  int n = stack.pop_smallint_range((stack.depth() - 1) / 2);
  st->consume_gas(VmState::bls_aggregate_verify_base_gas_price +
                  (long long)n * VmState::bls_aggregate_verify_element_gas_price);
  std::vector<std::pair<bls::P1, td::BufferSlice>> vec(n);
  for (int i = n - 1; i >= 0; --i) {
    vec[i].second = slice_to_bls_msg(*stack.pop_cellslice());
    vec[i].first = slice_to_bls_p1(*stack.pop_cellslice());
  }
  stack.push_bool(bls::aggregate_verify(vec, slice_to_bls_p2(*sig)));
  return 0;
}

int exec_bls_g1_add(VmState* st) {
  VM_LOG(st) << "execute BLS_G1_ADD";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  st->consume_gas(VmState::bls_g1_add_sub_gas_price);
  bls::P1 b = slice_to_bls_p1(*stack.pop_cellslice());
  bls::P1 a = slice_to_bls_p1(*stack.pop_cellslice());
  stack.push_cellslice(bls_to_slice(bls::g1_add(a, b).as_slice()));
  return 0;
}

int exec_bls_g1_sub(VmState* st) {
  VM_LOG(st) << "execute BLS_G1_SUB";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  st->consume_gas(VmState::bls_g1_add_sub_gas_price);
  bls::P1 b = slice_to_bls_p1(*stack.pop_cellslice());
  bls::P1 a = slice_to_bls_p1(*stack.pop_cellslice());
  stack.push_cellslice(bls_to_slice(bls::g1_sub(a, b).as_slice()));
  return 0;
}

int exec_bls_g1_neg(VmState* st) {
  VM_LOG(st) << "execute BLS_G1_NEG";
  Stack& stack = st->get_stack();
  st->consume_gas(VmState::bls_g1_neg_gas_price);
  bls::P1 a = slice_to_bls_p1(*stack.pop_cellslice());
  stack.push_cellslice(bls_to_slice(bls::g1_neg(a).as_slice()));
  return 0;
}

int exec_bls_g1_mul(VmState* st) {
  VM_LOG(st) << "execute BLS_G1_MUL";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  st->consume_gas(VmState::bls_g1_mul_gas_price);
  td::RefInt256 x = stack.pop_int_finite();
  bls::P1 p = slice_to_bls_p1(*stack.pop_cellslice());
  stack.push_cellslice(bls_to_slice(bls::g1_mul(p, x).as_slice()));
  return 0;
}

int exec_bls_g1_multiexp(VmState* st) {
  VM_LOG(st) << "execute BLS_G1_MULTIEXP";
  Stack& stack = st->get_stack();
  int n = stack.pop_smallint_range((stack.depth() - 1) / 2);
  st->consume_gas(bls_calculate_multiexp_gas(n, VmState::bls_g1_multiexp_base_gas_price,
                                             VmState::bls_g1_multiexp_coef1_gas_price,
                                             VmState::bls_g1_multiexp_coef2_gas_price));
  std::vector<std::pair<bls::P1, td::RefInt256>> ps(n);
  for (int i = n - 1; i >= 0; --i) {
    ps[i].second = stack.pop_int_finite();
    ps[i].first = slice_to_bls_p1(*stack.pop_cellslice());
  }
  stack.push_cellslice(bls_to_slice(bls::g1_multiexp(ps).as_slice()));
  return 0;
}

int exec_bls_g1_zero(VmState* st) {
  VM_LOG(st) << "execute BLS_G1_ZERO";
  Stack& stack = st->get_stack();
  stack.push_cellslice(bls_to_slice(bls::g1_zero().as_slice()));
  return 0;
}

int exec_bls_map_to_g1(VmState* st) {
  VM_LOG(st) << "execute BLS_MAP_TO_G1";
  Stack& stack = st->get_stack();
  st->consume_gas(VmState::bls_map_to_g1_gas_price);
  bls::FP a = slice_to_bls_fp(*stack.pop_cellslice());
  stack.push_cellslice(bls_to_slice(bls::map_to_g1(a).as_slice()));
  return 0;
}

int exec_bls_g1_in_group(VmState* st) {
  VM_LOG(st) << "execute BLS_G1_INGROUP";
  Stack& stack = st->get_stack();
  st->consume_gas(VmState::bls_g1_in_group_gas_price);
  bls::P1 a = slice_to_bls_p1(*stack.pop_cellslice());
  stack.push_bool(bls::g1_in_group(a));
  return 0;
}

int exec_bls_g1_is_zero(VmState* st) {
  VM_LOG(st) << "execute BLS_G1_ISZERO";
  Stack& stack = st->get_stack();
  bls::P1 a = slice_to_bls_p1(*stack.pop_cellslice());
  stack.push_bool(bls::g1_is_zero(a));
  return 0;
}

int exec_bls_g2_add(VmState* st) {
  VM_LOG(st) << "execute BLS_G2_ADD";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  st->consume_gas(VmState::bls_g2_add_sub_gas_price);
  bls::P2 b = slice_to_bls_p2(*stack.pop_cellslice());
  bls::P2 a = slice_to_bls_p2(*stack.pop_cellslice());
  stack.push_cellslice(bls_to_slice(bls::g2_add(a, b).as_slice()));
  return 0;
}

int exec_bls_g2_sub(VmState* st) {
  VM_LOG(st) << "execute BLS_G2_SUB";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  st->consume_gas(VmState::bls_g2_add_sub_gas_price);
  bls::P2 b = slice_to_bls_p2(*stack.pop_cellslice());
  bls::P2 a = slice_to_bls_p2(*stack.pop_cellslice());
  stack.push_cellslice(bls_to_slice(bls::g2_sub(a, b).as_slice()));
  return 0;
}

int exec_bls_g2_neg(VmState* st) {
  VM_LOG(st) << "execute BLS_G2_NEG";
  Stack& stack = st->get_stack();
  st->consume_gas(VmState::bls_g2_neg_gas_price);
  bls::P2 a = slice_to_bls_p2(*stack.pop_cellslice());
  stack.push_cellslice(bls_to_slice(bls::g2_neg(a).as_slice()));
  return 0;
}

int exec_bls_g2_mul(VmState* st) {
  VM_LOG(st) << "execute BLS_G2_MUL";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  st->consume_gas(VmState::bls_g2_mul_gas_price);
  td::RefInt256 x = stack.pop_int_finite();
  bls::P2 p = slice_to_bls_p2(*stack.pop_cellslice());
  stack.push_cellslice(bls_to_slice(bls::g2_mul(p, x).as_slice()));
  return 0;
}

int exec_bls_g2_multiexp(VmState* st) {
  VM_LOG(st) << "execute BLS_G2_MULTIEXP";
  Stack& stack = st->get_stack();
  int n = stack.pop_smallint_range((stack.depth() - 1) / 2);
  st->consume_gas(bls_calculate_multiexp_gas(n, VmState::bls_g2_multiexp_base_gas_price,
                                             VmState::bls_g2_multiexp_coef1_gas_price,
                                             VmState::bls_g2_multiexp_coef2_gas_price));
  std::vector<std::pair<bls::P2, td::RefInt256>> ps(n);
  for (int i = n - 1; i >= 0; --i) {
    ps[i].second = stack.pop_int_finite();
    ps[i].first = slice_to_bls_p2(*stack.pop_cellslice());
  }
  stack.push_cellslice(bls_to_slice(bls::g2_multiexp(ps).as_slice()));
  return 0;
}

int exec_bls_g2_zero(VmState* st) {
  VM_LOG(st) << "execute BLS_G2_ZERO";
  Stack& stack = st->get_stack();
  stack.push_cellslice(bls_to_slice(bls::g2_zero().as_slice()));
  return 0;
}

int exec_bls_map_to_g2(VmState* st) {
  VM_LOG(st) << "execute BLS_MAP_TO_G2";
  Stack& stack = st->get_stack();
  st->consume_gas(VmState::bls_map_to_g2_gas_price);
  bls::FP2 a = slice_to_bls_fp2(*stack.pop_cellslice());
  stack.push_cellslice(bls_to_slice(bls::map_to_g2(a).as_slice()));
  return 0;
}

int exec_bls_g2_in_group(VmState* st) {
  VM_LOG(st) << "execute BLS_G2_INGROUP";
  Stack& stack = st->get_stack();
  st->consume_gas(VmState::bls_g2_in_group_gas_price);
  bls::P2 a = slice_to_bls_p2(*stack.pop_cellslice());
  stack.push_bool(bls::g2_in_group(a));
  return 0;
}

int exec_bls_g2_is_zero(VmState* st) {
  VM_LOG(st) << "execute BLS_G2_ISZERO";
  Stack& stack = st->get_stack();
  bls::P2 a = slice_to_bls_p2(*stack.pop_cellslice());
  stack.push_bool(bls::g2_is_zero(a));
  return 0;
}

int exec_bls_pairing(VmState* st) {
  VM_LOG(st) << "execute BLS_PAIRING";
  Stack& stack = st->get_stack();
  int n = stack.pop_smallint_range((stack.depth() - 1) / 2);
  st->consume_gas(VmState::bls_pairing_base_gas_price + (long long)n * VmState::bls_pairing_element_gas_price);
  std::vector<std::pair<bls::P1, bls::P2>> ps(n);
  for (int i = n - 1; i >= 0; --i) {
    ps[i].second = slice_to_bls_p2(*stack.pop_cellslice());
    ps[i].first = slice_to_bls_p1(*stack.pop_cellslice());
  }
  stack.push_bool(bls::pairing(ps));
  return 0;
}

int exec_bls_push_r(VmState* st) {
  VM_LOG(st) << "execute BLS_PUSHR";
  Stack& stack = st->get_stack();
  stack.push_int(bls::get_r());
  return 0;
}

void register_ton_crypto_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xf900, 16, "HASHCU", std::bind(exec_compute_hash, _1, 0)))
      .insert(OpcodeInstr::mksimple(0xf901, 16, "HASHSU", std::bind(exec_compute_hash, _1, 1)))
      .insert(OpcodeInstr::mksimple(0xf902, 16, "SHA256U", exec_compute_sha256))
      .insert(OpcodeInstr::mkfixed(0xf904 >> 2, 14, 10, dump_hash_ext, exec_hash_ext)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf910, 16, "CHKSIGNU", std::bind(exec_ed25519_check_signature, _1, false)))
      .insert(OpcodeInstr::mksimple(0xf911, 16, "CHKSIGNS", std::bind(exec_ed25519_check_signature, _1, true)))
      .insert(OpcodeInstr::mksimple(0xf912, 16, "ECRECOVER", exec_ecrecover)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf913, 16, "SECP256K1_XONLY_PUBKEY_TWEAK_ADD", exec_secp256k1_xonly_pubkey_tweak_add)->require_version(9))
      .insert(OpcodeInstr::mksimple(0xf914, 16, "P256_CHKSIGNU", std::bind(exec_p256_chksign, _1, false))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf915, 16, "P256_CHKSIGNS", std::bind(exec_p256_chksign, _1, true))->require_version(4))

      .insert(OpcodeInstr::mksimple(0xf920, 16, "RIST255_FROMHASH", exec_ristretto255_from_hash)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf921, 16, "RIST255_VALIDATE", std::bind(exec_ristretto255_validate, _1, false))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf922, 16, "RIST255_ADD", std::bind(exec_ristretto255_add, _1, false))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf923, 16, "RIST255_SUB", std::bind(exec_ristretto255_sub, _1, false))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf924, 16, "RIST255_MUL", std::bind(exec_ristretto255_mul, _1, false))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf925, 16, "RIST255_MULBASE", std::bind(exec_ristretto255_mul_base, _1, false))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf926, 16, "RIST255_PUSHL", exec_ristretto255_push_l)->require_version(4))

      .insert(OpcodeInstr::mksimple(0xb7f921, 24, "RIST255_QVALIDATE", std::bind(exec_ristretto255_validate, _1, true))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xb7f922, 24, "RIST255_QADD", std::bind(exec_ristretto255_add, _1, true))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xb7f923, 24, "RIST255_QSUB", std::bind(exec_ristretto255_sub, _1, true))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xb7f924, 24, "RIST255_QMUL", std::bind(exec_ristretto255_mul, _1, true))->require_version(4))
      .insert(OpcodeInstr::mksimple(0xb7f925, 24, "RIST255_QMULBASE", std::bind(exec_ristretto255_mul_base, _1, true))->require_version(4))

      .insert(OpcodeInstr::mksimple(0xf93000, 24, "BLS_VERIFY", exec_bls_verify)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93001, 24, "BLS_AGGREGATE", exec_bls_aggregate)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93002, 24, "BLS_FASTAGGREGATEVERIFY", exec_bls_fast_aggregate_verify)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93003, 24, "BLS_AGGREGATEVERIFY", exec_bls_aggregate_verify)->require_version(4))

      .insert(OpcodeInstr::mksimple(0xf93010, 24, "BLS_G1_ADD", exec_bls_g1_add)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93011, 24, "BLS_G1_SUB", exec_bls_g1_sub)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93012, 24, "BLS_G1_NEG", exec_bls_g1_neg)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93013, 24, "BLS_G1_MUL", exec_bls_g1_mul)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93014, 24, "BLS_G1_MULTIEXP", exec_bls_g1_multiexp)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93015, 24, "BLS_G1_ZERO", exec_bls_g1_zero)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93016, 24, "BLS_MAP_TO_G1", exec_bls_map_to_g1)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93017, 24, "BLS_G1_INGROUP", exec_bls_g1_in_group)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93018, 24, "BLS_G1_ISZERO", exec_bls_g1_is_zero)->require_version(4))

      .insert(OpcodeInstr::mksimple(0xf93020, 24, "BLS_G2_ADD", exec_bls_g2_add)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93021, 24, "BLS_G2_SUB", exec_bls_g2_sub)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93022, 24, "BLS_G2_NEG", exec_bls_g2_neg)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93023, 24, "BLS_G2_MUL", exec_bls_g2_mul)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93024, 24, "BLS_G2_MULTIEXP", exec_bls_g2_multiexp)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93025, 24, "BLS_G2_ZERO", exec_bls_g2_zero)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93026, 24, "BLS_MAP_TO_G2", exec_bls_map_to_g2)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93027, 24, "BLS_G2_INGROUP", exec_bls_g2_in_group)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93028, 24, "BLS_G2_ISZERO", exec_bls_g2_is_zero)->require_version(4))

      .insert(OpcodeInstr::mksimple(0xf93030, 24, "BLS_PAIRING", exec_bls_pairing)->require_version(4))
      .insert(OpcodeInstr::mksimple(0xf93031, 24, "BLS_PUSHR", exec_bls_push_r)->require_version(4));
}

int exec_compute_data_size(VmState* st, int mode) {
  VM_LOG(st) << "execute " << (mode & 2 ? 'S' : 'C') << "DATASIZE" << (mode & 1 ? "Q" : "");
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  auto bound = stack.pop_int();
  Ref<Cell> cell;
  Ref<CellSlice> cs;
  if (mode & 2) {
    cs = stack.pop_cellslice();
  } else {
    cell = stack.pop_maybe_cell();
  }
  if (!bound->is_valid() || bound->sgn() < 0) {
    throw VmError{Excno::range_chk, "finite non-negative integer expected"};
  }
  VmStorageStat stat{bound->unsigned_fits_bits(63) ? bound->to_long() : (1ULL << 63) - 1};
  bool ok = (mode & 2 ? stat.add_storage(cs.write()) : stat.add_storage(std::move(cell)));
  if (ok) {
    stack.push_smallint(stat.cells);
    stack.push_smallint(stat.bits);
    stack.push_smallint(stat.refs);
  } else if (!(mode & 1)) {
    throw VmError{Excno::cell_ov, "scanned too many cells"};
  }
  if (mode & 1) {
    stack.push_bool(ok);
  }
  return 0;
}

void register_ton_misc_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xf940, 16, "CDATASIZEQ", std::bind(exec_compute_data_size, _1, 1)))
      .insert(OpcodeInstr::mksimple(0xf941, 16, "CDATASIZE", std::bind(exec_compute_data_size, _1, 0)))
      .insert(OpcodeInstr::mksimple(0xf942, 16, "SDATASIZEQ", std::bind(exec_compute_data_size, _1, 3)))
      .insert(OpcodeInstr::mksimple(0xf943, 16, "SDATASIZE", std::bind(exec_compute_data_size, _1, 2)));
}

int exec_load_var_integer(VmState* st, int len_bits, bool sgnd, bool quiet) {
  if (len_bits == 4 && !sgnd) {
    VM_LOG(st) << "execute LDGRAMS" << (quiet ? "Q" : "");
  } else {
    VM_LOG(st) << "execute LDVAR" << (sgnd ? "" : "U") << "INT" << (1 << len_bits) << (quiet ? "Q" : "");
  }
  Stack& stack = st->get_stack();
  auto csr = stack.pop_cellslice();
  td::RefInt256 x;
  if (util::load_var_integer_q(csr.write(), x, len_bits, sgnd, quiet)) {
    stack.push_int(std::move(x));
    stack.push_cellslice(std::move(csr));
    if (quiet) {
      stack.push_bool(true);
    }
  } else {
    stack.push_bool(false);
  }
  return 0;
}

int exec_store_var_integer(VmState* st, int len_bits, bool sgnd, bool quiet) {
  if (len_bits == 4 && !sgnd) {
    VM_LOG(st) << "execute STGRAMS" << (quiet ? "Q" : "");
  } else {
    VM_LOG(st) << "execute STVAR" << (sgnd ? "" : "U") << "INT" << (1 << len_bits) << (quiet ? "Q" : "");
  }
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  auto x = stack.pop_int();
  auto cbr = stack.pop_builder();
  if (util::store_var_integer(cbr.write(), x, len_bits, sgnd, quiet)) {
    stack.push_builder(std::move(cbr));
    if (quiet) {
      stack.push_bool(true);
    }
  } else {
    stack.push_bool(false);
  }
  return 0;
}

bool skip_maybe_anycast(CellSlice& cs, int global_version) {
  if (cs.prefetch_ulong(1) != 1) {
    return cs.advance(1);
  }
  if (global_version >= 10) {
    return false;
  }
  unsigned depth;
  return cs.advance(1)                    // just$1
         && cs.fetch_uint_leq(30, depth)  // anycast_info$_ depth:(#<= 30)
         && depth >= 1                    // { depth >= 1 }
         && cs.advance(depth);            // rewrite_pfx:(bits depth) = Anycast;
}

bool skip_message_addr(CellSlice& cs, int global_version) {
  switch ((unsigned)cs.fetch_ulong(2)) {
    case 0:  // addr_none$00 = MsgAddressExt;
      return true;
    case 1: {  // addr_extern$01
      unsigned len;
      return cs.fetch_uint_to(9, len)  // len:(## 9)
             && cs.advance(len);       // external_address:(bits len) = MsgAddressExt;
    }
    case 2: {                                        // addr_std$10
      return skip_maybe_anycast(cs, global_version)  // anycast:(Maybe Anycast)
             && cs.advance(8 + 256);                 // workchain_id:int8 address:bits256  = MsgAddressInt;
    }
    case 3: {  // addr_var$11
      if (global_version >= 10) {
        return false;
      }
      unsigned len;
      return skip_maybe_anycast(cs, global_version)  // anycast:(Maybe Anycast)
             && cs.fetch_uint_to(9, len)             // addr_len:(## 9)
             && cs.advance(32 + len);                // workchain_id:int32 address:(bits addr_len) = MsgAddressInt;
    }
    default:
      return false;
  }
}

int exec_load_message_addr(VmState* st, bool quiet) {
  VM_LOG(st) << "execute LDMSGADDR" << (quiet ? "Q" : "");
  Stack& stack = st->get_stack();
  auto csr = stack.pop_cellslice();
  td::Ref<CellSlice> addr{true};
  if (util::load_msg_addr_q(csr.write(), addr.write(), st->get_global_version(), quiet)) {
    stack.push_cellslice(std::move(addr));
    stack.push_cellslice(std::move(csr));
    if (quiet) {
      stack.push_bool(true);
    }
  } else {
    stack.push_cellslice(std::move(csr));
    stack.push_bool(false);
  }
  return 0;
}

bool parse_maybe_anycast(CellSlice& cs, StackEntry& res, int global_version) {
  res = StackEntry{};
  if (cs.prefetch_ulong(1) != 1) {
    return cs.advance(1);
  }
  if (global_version >= 10) {
    return false;
  }
  unsigned depth;
  Ref<CellSlice> pfx;
  if (cs.advance(1)                           // just$1
      && cs.fetch_uint_leq(30, depth)         // anycast_info$_ depth:(#<= 30)
      && depth >= 1                           // { depth >= 1 }
      && cs.fetch_subslice_to(depth, pfx)) {  // rewrite_pfx:(bits depth) = Anycast;
    res = std::move(pfx);
    return true;
  }
  return false;
}

bool parse_message_addr(CellSlice& cs, std::vector<StackEntry>& res, int global_version) {
  res.clear();
  switch ((unsigned)cs.fetch_ulong(2)) {
    case 0:                                 // addr_none$00 = MsgAddressExt;
      res.emplace_back(td::zero_refint());  // -> (0)
      return true;
    case 1: {  // addr_extern$01
      unsigned len;
      Ref<CellSlice> addr;
      if (cs.fetch_uint_to(9, len)               // len:(## 9)
          && cs.fetch_subslice_to(len, addr)) {  // external_address:(bits len) = MsgAddressExt;
        res.emplace_back(td::make_refint(1));
        res.emplace_back(std::move(addr));
        return true;
      }
      break;
    }
    case 2: {  // addr_std$10
      StackEntry v;
      int workchain;
      Ref<CellSlice> addr;
      if (parse_maybe_anycast(cs, v, global_version)  // anycast:(Maybe Anycast)
          && cs.fetch_int_to(8, workchain)            // workchain_id:int8
          && cs.fetch_subslice_to(256, addr)) {       // address:bits256  = MsgAddressInt;
        res.emplace_back(td::make_refint(2));
        res.emplace_back(std::move(v));
        res.emplace_back(td::make_refint(workchain));
        res.emplace_back(std::move(addr));
        return true;
      }
      break;
    }
    case 3: {  // addr_var$11
      if (global_version >= 10) {
        return false;
      }
      StackEntry v;
      int len, workchain;
      Ref<CellSlice> addr;
      if (parse_maybe_anycast(cs, v, global_version)  // anycast:(Maybe Anycast)
          && cs.fetch_uint_to(9, len)                 // addr_len:(## 9)
          && cs.fetch_int_to(32, workchain)           // workchain_id:int32
          && cs.fetch_subslice_to(len, addr)) {       // address:(bits addr_len) = MsgAddressInt;
        res.emplace_back(td::make_refint(3));
        res.emplace_back(std::move(v));
        res.emplace_back(td::make_refint(workchain));
        res.emplace_back(std::move(addr));
        return true;
      }
      break;
    }
  }
  return false;
}

int exec_parse_message_addr(VmState* st, bool quiet) {
  VM_LOG(st) << "execute PARSEMSGADDR" << (quiet ? "Q" : "");
  Stack& stack = st->get_stack();
  auto csr = stack.pop_cellslice();
  auto& cs = csr.write();
  std::vector<StackEntry> res;
  if (!(parse_message_addr(cs, res, st->get_global_version()) && cs.empty_ext())) {
    if (quiet) {
      stack.push_bool(false);
    } else {
      throw VmError{Excno::cell_und, "cannot parse a MsgAddress"};
    }
  } else {
    stack.push_tuple(std::move(res));
    if (quiet) {
      stack.push_bool(true);
    }
  }
  return 0;
}

// replaces first bits of `addr` with those of `prefix`
Ref<CellSlice> do_rewrite_addr(Ref<CellSlice> addr, Ref<CellSlice> prefix) {
  if (prefix.is_null() || !prefix->size()) {
    return std::move(addr);
  }
  if (prefix->size() > addr->size()) {
    return {};
  }
  if (prefix->size() == addr->size()) {
    return std::move(prefix);
  }
  vm::CellBuilder cb;
  if (!(addr.write().advance(prefix->size()) && cb.append_cellslice_bool(std::move(prefix)) &&
        cb.append_cellslice_bool(std::move(addr)))) {
    return {};
  }
  return vm::load_cell_slice_ref(cb.finalize());
}

int exec_rewrite_message_addr(VmState* st, bool allow_var_addr, bool quiet) {
  VM_LOG(st) << "execute REWRITE" << (allow_var_addr ? "VAR" : "STD") << "ADDR" << (quiet ? "Q" : "");
  Stack& stack = st->get_stack();
  auto csr = stack.pop_cellslice();
  auto& cs = csr.write();
  std::vector<StackEntry> tuple;
  if (!(parse_message_addr(cs, tuple, st->get_global_version()) && cs.empty_ext())) {
    if (quiet) {
      stack.push_bool(false);
      return 0;
    }
    throw VmError{Excno::cell_und, "cannot parse a MsgAddress"};
  }
  int t = (int)std::move(tuple[0]).as_int()->to_long();
  if (t != 2 && t != 3) {
    if (quiet) {
      stack.push_bool(false);
      return 0;
    }
    throw VmError{Excno::cell_und, "cannot parse a MsgAddressInt"};
  }
  auto addr = std::move(tuple[3]).as_slice();
  auto prefix = std::move(tuple[1]).as_slice();
  if (!allow_var_addr) {
    if (addr->size() != 256) {
      if (quiet) {
        stack.push_bool(false);
        return 0;
      }
      throw VmError{Excno::cell_und, "MsgAddressInt is not a standard 256-bit address"};
    }
    td::Bits256 rw_addr;
    td::RefInt256 int_addr{true};
    CHECK(addr->prefetch_bits_to(rw_addr) &&
          (prefix.is_null() || prefix->prefetch_bits_to(rw_addr.bits(), prefix->size())) &&
          int_addr.unique_write().import_bits(rw_addr, false));
    stack.push(std::move(tuple[2]));
    stack.push(std::move(int_addr));
  } else {
    addr = do_rewrite_addr(std::move(addr), std::move(prefix));
    if (addr.is_null()) {
      if (quiet) {
        stack.push_bool(false);
        return 0;
      }
      throw VmError{Excno::cell_und, "cannot rewrite address in a MsgAddressInt"};
    }
    stack.push(std::move(tuple[2]));
    stack.push(std::move(addr));
  }
  if (quiet) {
    stack.push_bool(true);
  }
  return 0;
}

void register_ton_currency_address_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xfa00, 16, "LDGRAMS", std::bind(exec_load_var_integer, _1, 4, false, false)))
      .insert(OpcodeInstr::mksimple(0xfa01, 16, "LDVARINT16", std::bind(exec_load_var_integer, _1, 4, true, false)))
      .insert(OpcodeInstr::mksimple(0xfa02, 16, "STGRAMS", std::bind(exec_store_var_integer, _1, 4, false, false)))
      .insert(OpcodeInstr::mksimple(0xfa03, 16, "STVARINT16", std::bind(exec_store_var_integer, _1, 4, true, false)))
      .insert(OpcodeInstr::mksimple(0xfa04, 16, "LDVARUINT32", std::bind(exec_load_var_integer, _1, 5, false, false)))
      .insert(OpcodeInstr::mksimple(0xfa05, 16, "LDVARINT32", std::bind(exec_load_var_integer, _1, 5, true, false)))
      .insert(OpcodeInstr::mksimple(0xfa06, 16, "STVARUINT32", std::bind(exec_store_var_integer, _1, 5, false, false)))
      .insert(OpcodeInstr::mksimple(0xfa07, 16, "STVARINT32", std::bind(exec_store_var_integer, _1, 5, true, false)))
      .insert(OpcodeInstr::mksimple(0xfa40, 16, "LDMSGADDR", std::bind(exec_load_message_addr, _1, false)))
      .insert(OpcodeInstr::mksimple(0xfa41, 16, "LDMSGADDRQ", std::bind(exec_load_message_addr, _1, true)))
      .insert(OpcodeInstr::mksimple(0xfa42, 16, "PARSEMSGADDR", std::bind(exec_parse_message_addr, _1, false)))
      .insert(OpcodeInstr::mksimple(0xfa43, 16, "PARSEMSGADDRQ", std::bind(exec_parse_message_addr, _1, true)))
      .insert(
          OpcodeInstr::mksimple(0xfa44, 16, "REWRITESTDADDR", std::bind(exec_rewrite_message_addr, _1, false, false)))
      .insert(
          OpcodeInstr::mksimple(0xfa45, 16, "REWRITESTDADDRQ", std::bind(exec_rewrite_message_addr, _1, false, true)))
      .insert(
          OpcodeInstr::mksimple(0xfa46, 16, "REWRITEVARADDR", std::bind(exec_rewrite_message_addr, _1, true, false)))
      .insert(
          OpcodeInstr::mksimple(0xfa47, 16, "REWRITEVARADDRQ", std::bind(exec_rewrite_message_addr, _1, true, true)));
}

static constexpr int output_actions_idx = 5;

int install_output_action(VmState* st, Ref<Cell> new_action_head) {
  // TODO: increase actions:uint16 and msgs_sent:uint16 in SmartContractInfo at first reference of c5
  VM_LOG(st) << "installing an output action";
  st->set_d(output_actions_idx, std::move(new_action_head));
  return 0;
}

static inline Ref<Cell> get_actions(VmState* st) {
  return st->get_d(output_actions_idx);
}

int exec_send_raw_message(VmState* st) {
  VM_LOG(st) << "execute SENDRAWMSG";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  int f = stack.pop_smallint_range(255);
  Ref<Cell> msg_cell = stack.pop_cell();
  CellBuilder cb;
  if (!(cb.store_ref_bool(get_actions(st))     // out_list$_ {n:#} prev:^(OutList n)
        && cb.store_long_bool(0x0ec3c86d, 32)  // action_send_msg#0ec3c86d
        && cb.store_long_bool(f, 8)            // mode:(## 8)
        && cb.store_ref_bool(std::move(msg_cell)))) {
    throw VmError{Excno::cell_ov, "cannot serialize raw output message into an output action cell"};
  }
  return install_output_action(st, cb.finalize());
}

int parse_addr_workchain(CellSlice cs) {
  // anycast_info$_ depth:(#<= 30) { depth >= 1 } rewrite_pfx:(bits depth) = Anycast;
  // addr_std$10 anycast:(Maybe Anycast) workchain_id:int8 address:bits256  = MsgAddressInt;
  // addr_var$11 anycast:(Maybe Anycast) addr_len:(## 9) workchain_id:int32 address:(bits addr_len) = MsgAddressInt;
  if (cs.fetch_ulong(1) != 1) {
    throw VmError{Excno::range_chk, "not an internal MsgAddress"};
  }
  bool is_var = cs.fetch_ulong(1);
  if (cs.fetch_ulong(1) == 1) { // Anycast
    unsigned depth;
    cs.fetch_uint_leq(30, depth);
    cs.skip_first(depth);
  }

  if (is_var) {
    cs.skip_first(9);
    return (int)cs.fetch_long(32);
  } else {
    return (int)cs.fetch_long(8);
  }
}

int exec_send_message(VmState* st) {
  VM_LOG(st) << "execute SENDMSG";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  int mode = stack.pop_smallint_range(2047);
  bool send = !(mode & 1024);
  mode &= ~1024;
  if (mode >= 256) {
    throw VmError{Excno::range_chk};
  }
  Ref<Cell> msg_cell = stack.pop_cell();

  block::gen::MessageRelaxed::Record msg;
  if (!tlb::type_unpack_cell(msg_cell, block::gen::t_MessageRelaxed_Any, msg)) {
    throw VmError{Excno::unknown, "invalid message"};
  }

  Ref<CellSlice> my_addr = get_param(st, 8).as_slice();
  if (my_addr.is_null()) {
    throw VmError{Excno::type_chk, "invalid param MYADDR"};
  }
  bool ihr_disabled;
  Ref<CellSlice> dest;
  td::RefInt256 value;
  td::RefInt256 user_fwd_fee, user_ihr_fee;
  bool have_extra_currencies = false;
  bool ext_msg = msg.info->prefetch_ulong(1);
  if (ext_msg) { // External message
    block::gen::CommonMsgInfoRelaxed::Record_ext_out_msg_info info;
    if (!tlb::csr_unpack(msg.info, info)) {
      throw VmError{Excno::unknown, "invalid message"};
    }
    ihr_disabled = true;
    dest = std::move(info.dest);
    value = user_fwd_fee = user_ihr_fee = td::zero_refint();
  } else { // Internal message
    block::gen::CommonMsgInfoRelaxed::Record_int_msg_info info;
    if (!tlb::csr_unpack(msg.info, info)) {
      throw VmError{Excno::unknown, "invalid message"};
    }
    ihr_disabled = info.ihr_disabled;
    dest = std::move(info.dest);
    Ref<vm::Cell> extra;
    if (!block::tlb::t_CurrencyCollection.unpack_special(info.value.write(), value, extra)) {
      throw VmError{Excno::unknown, "invalid message"};
    }
    have_extra_currencies = !extra.is_null();
    user_fwd_fee = block::tlb::t_Grams.as_integer(info.fwd_fee);
    user_ihr_fee = block::tlb::t_Grams.as_integer(info.ihr_fee);
  }

  bool is_masterchain = parse_addr_workchain(*my_addr) == -1 || (!ext_msg && parse_addr_workchain(*dest) == -1);
  td::Ref<CellSlice> prices_cs;
  if (st->get_global_version() >= 6) {
    prices_cs = tuple_index(get_unpacked_config_tuple(st), is_masterchain ? 4 : 5).as_slice();
  } else {
    Ref<Cell> config_dict = get_param(st, 9).as_cell();
    Dictionary config{config_dict, 32};
    Ref<Cell> prices_cell = config.lookup_ref(td::BitArray<32>{is_masterchain ? 24 : 25});
    if (prices_cell.not_null()) {
      prices_cs = load_cell_slice_ref(prices_cell);
    }
  }
  if (prices_cs.is_null()) {
    throw VmError{Excno::unknown, "invalid prices config"};
  }
  auto r_prices = block::Config::do_get_msg_prices(*prices_cs, is_masterchain ? 24 : 25);
  if (r_prices.is_error()) {
    throw VmError{Excno::cell_und, PSTRING() << "cannot parse config: " << r_prices.error().message()};
  }
  block::MsgPrices prices = r_prices.move_as_ok();

  // msg_fwd_fees = (lump_price + ceil((bit_price * msg.bits + cell_price * msg.cells)/2^16)) nanograms
  // bits in the root cell of a message are not included in msg.bits (lump_price pays for them)
  td::uint64 max_cells;
  if (st->get_global_version() >= 6) {
    auto r_size_limits_config =
        block::Config::do_get_size_limits_config(tuple_index(get_unpacked_config_tuple(st), 6).as_slice());
    if (r_size_limits_config.is_error()) {
      throw VmError{Excno::cell_und, PSTRING() << "cannot parse config: " << r_size_limits_config.error().message()};
    }
    max_cells = r_size_limits_config.ok().max_msg_cells;
  } else {
    max_cells = 1 << 13;
  }
  vm::VmStorageStat stat(max_cells);
  CellSlice cs = load_cell_slice(msg_cell);
  cs.skip_first(cs.size());
  if (st->get_global_version() >= 10 && have_extra_currencies) {
    // Skip extra currency dict
    cs.advance_refs(1);
  }
  stat.add_storage(cs);

  if (!ext_msg) {
    if (mode & 128) {  // value is balance of the contract
      Ref<Tuple> balance = get_param(st, 7).as_tuple();
      if (balance.is_null()) {
        throw VmError{Excno::type_chk, "invalid param BALANCE"};
      }
      value = tuple_index(balance, 0).as_int();
      if (value.is_null()) {
        throw VmError{Excno::type_chk, "invalid param BALANCE"};
      }
      if (st->get_global_version() < 10) {
        have_extra_currencies |= !tuple_index(balance, 1).as_cell().is_null();
      }
    } else if (mode & 64) {  // value += value of incoming message
      Ref<Tuple> balance = get_param(st, 11).as_tuple();
      if (balance.is_null()) {
        throw VmError{Excno::type_chk, "invalid param INCOMINGVALUE"};
      }
      td::RefInt256 balance_grams = tuple_index(balance, 0).as_int();
      if (balance_grams.is_null()) {
        throw VmError{Excno::type_chk, "invalid param INCOMINGVALUE"};
      }
      value += balance_grams;
      if (st->get_global_version() < 10) {
        have_extra_currencies |= !tuple_index(balance, 1).as_cell().is_null();
      }
    }
  }

  bool have_init = msg.init->bit_at(0);
  bool init_ref = have_init && msg.init->bit_at(1);
  bool body_ref = msg.body->bit_at(0);

  td::RefInt256 fwd_fee, ihr_fee;
  td::uint64 cells = stat.cells;
  td::uint64 bits = stat.bits;
  auto compute_fees = [&]() {
    td::uint64 fwd_fee_short = prices.lump_price + td::uint128(prices.bit_price)
                                                 .mult(bits)
                                                 .add(td::uint128(prices.cell_price).mult(cells))
                                                 .add(td::uint128(0xffffu))
                                                 .shr(16)
                                                 .lo();
    td::uint64 ihr_fee_short;
    if (ihr_disabled) {
      ihr_fee_short = 0;
    } else {
      ihr_fee_short = td::uint128(fwd_fee_short).mult(prices.ihr_factor).shr(16).lo();
    }
    fwd_fee = td::RefInt256{true, fwd_fee_short};
    ihr_fee = td::RefInt256{true, ihr_fee_short};
    fwd_fee = std::max(fwd_fee, user_fwd_fee);
    if (!ihr_disabled) {
      ihr_fee = std::max(ihr_fee, user_ihr_fee);
    }
  };
  compute_fees();

  auto stored_grams_len = [](td::RefInt256 const& x) -> unsigned {
    unsigned bits = x->bit_size(false);
    return 4 + ((bits + 7) & ~7);
  };

  auto msg_root_bits = [&]() -> unsigned {
    unsigned bits;
    // CommonMsgInfo
    if (ext_msg) {
      bits = 2 + my_addr->size() + dest->size() + 32 + 64;
    } else {
      bits = 4 + my_addr->size() + dest->size() + stored_grams_len(value) + 1 + 32 + 64;
      td::RefInt256 fwd_fee_first = (fwd_fee * prices.first_frac) >> 16;
      bits += stored_grams_len(fwd_fee - fwd_fee_first);
      bits += stored_grams_len(ihr_fee);
    }
    // init
    bits++;
    if (have_init) {
      bits += 1 + (init_ref ? 0 : msg.init->size() - 2);
    }
    // body
    bits++;
    bits += (body_ref ? 0 : msg.body->size() - 1);
    return bits;
  };
  auto msg_root_refs = [&]() -> unsigned {
    unsigned refs;
    // CommonMsgInfo
    if (ext_msg) {
      refs = 0;
    } else {
      refs = have_extra_currencies;
    }
    // init
    if (have_init) {
      refs += (init_ref ? 1 : msg.init->size_refs());
    }
    // body
    refs += (body_ref ? 1 : msg.body->size_refs());
    return refs;
  };

  if (have_init && !init_ref && (msg_root_bits() > Cell::max_bits || msg_root_refs() > Cell::max_refs)) {
    init_ref = true;
    cells += 1;
    bits += msg.init->size() - 2;
    compute_fees();
  }
  if (!body_ref && (msg_root_bits() > Cell::max_bits || msg_root_refs() > Cell::max_refs)) {
    body_ref = true;
    cells += 1;
    bits += msg.body->size() - 1;
    compute_fees();
  }
  stack.push_int(fwd_fee + ihr_fee);

  if (send) {
    CellBuilder cb;
    if (!(cb.store_ref_bool(get_actions(st))     // out_list$_ {n:#} prev:^(OutList n)
          && cb.store_long_bool(0x0ec3c86d, 32)  // action_send_msg#0ec3c86d
          && cb.store_long_bool(mode, 8)         // mode:(## 8)
          && cb.store_ref_bool(std::move(msg_cell)))) {
      throw VmError{Excno::cell_ov, "cannot serialize raw output message into an output action cell"};
    }
    return install_output_action(st, cb.finalize());
  }
  return 0;
}

bool store_grams(CellBuilder& cb, td::RefInt256 value) {
  int k = value->bit_size(false);
  return k <= 15 * 8 && cb.store_long_bool((k + 7) >> 3, 4) && cb.store_int256_bool(*value, (k + 7) & -8, false);
}

int exec_reserve_raw(VmState* st, int mode) {
  VM_LOG(st) << "execute RAWRESERVE" << (mode & 1 ? "X" : "");
  Stack& stack = st->get_stack();
  stack.check_underflow(2 + (mode & 1));
  int f = stack.pop_smallint_range(st->get_global_version() >= 4 ? 31 : 15);
  Ref<Cell> y;
  if (mode & 1) {
    y = stack.pop_maybe_cell();
  }
  auto x = stack.pop_int_finite();
  if (td::sgn(x) < 0) {
    throw VmError{Excno::range_chk, "amount of nanograms must be non-negative"};
  }
  CellBuilder cb;
  if (!(cb.store_ref_bool(get_actions(st))     // out_list$_ {n:#} prev:^(OutList n)
        && cb.store_long_bool(0x36e6b809, 32)  // action_reserve_currency#36e6b809
        && cb.store_long_bool(f, 8)            // mode:(## 8)
        && store_grams(cb, std::move(x))       //
        && cb.store_maybe_ref(std::move(y)))) {
    throw VmError{Excno::cell_ov, "cannot serialize raw reserved currency amount into an output action cell"};
  }
  return install_output_action(st, cb.finalize());
}

int exec_set_code(VmState* st) {
  VM_LOG(st) << "execute SETCODE";
  auto code = st->get_stack().pop_cell();
  CellBuilder cb;
  if (!(cb.store_ref_bool(get_actions(st))         // out_list$_ {n:#} prev:^(OutList n)
        && cb.store_long_bool(0xad4de08e, 32)      // action_set_code#ad4de08e
        && cb.store_ref_bool(std::move(code)))) {  // new_code:^Cell = OutAction;
    throw VmError{Excno::cell_ov, "cannot serialize new smart contract code into an output action cell"};
  }
  return install_output_action(st, cb.finalize());
}

int exec_set_lib_code(VmState* st) {
  VM_LOG(st) << "execute SETLIBCODE";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  int mode;
  if (st->get_global_version() >= 4) {
    mode = stack.pop_smallint_range(31);
    if ((mode & ~16) > 2) {
      throw VmError{Excno::range_chk};
    }
  } else {
    mode = stack.pop_smallint_range(2);
  }
  auto code = stack.pop_cell();
  CellBuilder cb;
  if (!(cb.store_ref_bool(get_actions(st))         // out_list$_ {n:#} prev:^(OutList n)
        && cb.store_long_bool(0x26fa1dd4, 32)      // action_change_library#26fa1dd4
        && cb.store_long_bool(mode * 2 + 1, 8)     // mode:(## 7)
        && cb.store_ref_bool(std::move(code)))) {  // libref:LibRef = OutAction;
    throw VmError{Excno::cell_ov, "cannot serialize new library code into an output action cell"};
  }
  return install_output_action(st, cb.finalize());
}

int exec_change_lib(VmState* st) {
  VM_LOG(st) << "execute CHANGELIB";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  int mode;
  if (st->get_global_version() >= 4) {
    mode = stack.pop_smallint_range(31);
    if ((mode & ~16) > 2) {
      throw VmError{Excno::range_chk};
    }
  } else {
    mode = stack.pop_smallint_range(2);
  }
  auto hash = stack.pop_int_finite();
  if (!hash->unsigned_fits_bits(256)) {
    throw VmError{Excno::range_chk, "library hash must be non-negative"};
  }
  CellBuilder cb;
  if (!(cb.store_ref_bool(get_actions(st))             // out_list$_ {n:#} prev:^(OutList n)
        && cb.store_long_bool(0x26fa1dd4, 32)          // action_change_library#26fa1dd4
        && cb.store_long_bool(mode * 2, 8)             // mode:(## 7) { mode <= 2 }
        && cb.store_int256_bool(hash, 256, false))) {  // libref:LibRef = OutAction;
    throw VmError{Excno::cell_ov, "cannot serialize library hash into an output action cell"};
  }
  return install_output_action(st, cb.finalize());
}

void register_ton_message_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xfb00, 16, "SENDRAWMSG", exec_send_raw_message))
      .insert(OpcodeInstr::mksimple(0xfb02, 16, "RAWRESERVE", std::bind(exec_reserve_raw, _1, 0)))
      .insert(OpcodeInstr::mksimple(0xfb03, 16, "RAWRESERVEX", std::bind(exec_reserve_raw, _1, 1)))
      .insert(OpcodeInstr::mksimple(0xfb04, 16, "SETCODE", exec_set_code))
      .insert(OpcodeInstr::mksimple(0xfb06, 16, "SETLIBCODE", exec_set_lib_code))
      .insert(OpcodeInstr::mksimple(0xfb07, 16, "CHANGELIB", exec_change_lib))
      .insert(OpcodeInstr::mksimple(0xfb08, 16, "SENDMSG", exec_send_message)->require_version(4));
}

void register_ton_ops(OpcodeTable& cp0) {
  register_basic_gas_ops(cp0);
  register_ton_gas_ops(cp0);
  register_prng_ops(cp0);
  register_ton_config_ops(cp0);
  register_ton_crypto_ops(cp0);
  register_ton_misc_ops(cp0);
  register_ton_currency_address_ops(cp0);
  register_ton_message_ops(cp0);
}

namespace util {

bool load_var_integer_q(CellSlice& cs, td::RefInt256& res, int len_bits, bool sgnd, bool quiet) {
  CellSlice cs0 = cs;
  int len;
  if (!(cs.fetch_uint_to(len_bits, len) && cs.fetch_int256_to(len * 8, res, sgnd))) {
    cs = std::move(cs0);
    if (quiet) {
      return false;
    }
    throw VmError{Excno::cell_und, "cannot deserialize a variable-length integer"};
  }
  return true;
}
bool load_coins_q(CellSlice& cs, td::RefInt256& res, bool quiet) {
  return load_var_integer_q(cs, res, 4, false, quiet);
}
bool load_msg_addr_q(CellSlice& cs, CellSlice& res, int global_version, bool quiet) {
  res = cs;
  if (!skip_message_addr(cs, global_version)) {
    cs = res;
    if (quiet) {
      return false;
    }
    throw VmError{Excno::cell_und, "cannot load a MsgAddress"};
  }
  res.cut_tail(cs);
  return true;
}
bool parse_std_addr_q(CellSlice cs, ton::WorkchainId& res_wc, ton::StdSmcAddress& res_addr, int global_version,
                      bool quiet) {
  // Like exec_rewrite_message_addr, but for std address case
  std::vector<StackEntry> tuple;
  if (!(parse_message_addr(cs, tuple, global_version) && cs.empty_ext())) {
    if (quiet) {
      return false;
    }
    throw VmError{Excno::cell_und, "cannot parse a MsgAddress"};
  }
  int t = (int)std::move(tuple[0]).as_int()->to_long();
  if (t != 2 && t != 3) {
    if (quiet) {
      return false;
    }
    throw VmError{Excno::cell_und, "cannot parse a MsgAddressInt"};
  }
  auto addr = std::move(tuple[3]).as_slice();
  auto prefix = std::move(tuple[1]).as_slice();
  if (addr->size() != 256) {
    if (quiet) {
      return false;
    }
    throw VmError{Excno::cell_und, "MsgAddressInt is not a standard 256-bit address"};
  }
  res_wc = (int)tuple[2].as_int()->to_long();
  CHECK(addr->prefetch_bits_to(res_addr) &&
        (prefix.is_null() || prefix->prefetch_bits_to(res_addr.bits(), prefix->size())));
  return true;
}

td::RefInt256 load_var_integer(CellSlice& cs, int len_bits, bool sgnd) {
  td::RefInt256 x;
  load_var_integer_q(cs, x, len_bits, sgnd, false);
  return x;
}
td::RefInt256 load_coins(CellSlice& cs) {
  return load_var_integer(cs, 4, false);
}
CellSlice load_msg_addr(CellSlice& cs, int global_version) {
  CellSlice addr;
  load_msg_addr_q(cs, addr, global_version, false);
  return addr;
}
std::pair<ton::WorkchainId, ton::StdSmcAddress> parse_std_addr(CellSlice cs, int global_version) {
  std::pair<ton::WorkchainId, ton::StdSmcAddress> res;
  parse_std_addr_q(std::move(cs), res.first, res.second, global_version, false);
  return res;
}

bool store_var_integer(CellBuilder& cb, const td::RefInt256& x, int len_bits, bool sgnd, bool quiet) {
  unsigned len = (((unsigned)x->bit_size(sgnd) + 7) >> 3);
  if (len >= (1u << len_bits)) {
    throw VmError{Excno::range_chk};  // throw even if quiet
  }
  if (!cb.can_extend_by(len_bits + len * 8)) {
    if (quiet) {
      return false;
    }
    throw VmError{Excno::cell_ov, "cannot serialize a variable-length integer"};
  }
  CHECK(cb.store_long_bool(len, len_bits) && cb.store_int256_bool(*x, len * 8, sgnd));
  return true;
}
bool store_coins(CellBuilder& cb, const td::RefInt256& x, bool quiet) {
  return store_var_integer(cb, x, 4, false, quiet);
}

block::GasLimitsPrices get_gas_prices(const Ref<Tuple>& unpacked_config, bool is_masterchain) {
  Ref<CellSlice> cs = tuple_index(unpacked_config, is_masterchain ? 2 : 3).as_slice();
  if (cs.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a slice"};
  }
  auto r_prices = block::Config::do_get_gas_limits_prices(*cs, is_masterchain ? 20 : 21);
  if (r_prices.is_error()) {
    throw VmError{Excno::cell_und, PSTRING() << "cannot parse config: " << r_prices.error().message()};
  }
  return r_prices.move_as_ok();
}

block::MsgPrices get_msg_prices(const Ref<Tuple>& unpacked_config, bool is_masterchain) {
  Ref<CellSlice> cs = tuple_index(unpacked_config, is_masterchain ? 4 : 5).as_slice();
  if (cs.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a slice"};
  }
  auto r_prices = block::Config::do_get_msg_prices(*cs, is_masterchain ? 24 : 25);
  if (r_prices.is_error()) {
    throw VmError{Excno::cell_und, PSTRING() << "cannot parse config: " << r_prices.error().message()};
  }
  return r_prices.move_as_ok();
}

td::optional<block::StoragePrices> get_storage_prices(const Ref<Tuple>& unpacked_config) {
  Ref<CellSlice> cs = tuple_index(unpacked_config, 0).as_slice();
  if (cs.is_null()) {
    // null means tat no StoragePrices is active, so the price is 0
    return {};
  }
  auto r_prices = block::Config::do_get_one_storage_prices(*cs);
  if (r_prices.is_error()) {
    throw VmError{Excno::cell_und, PSTRING() << "cannot parse config: " << r_prices.error().message()};
  }
  return r_prices.move_as_ok();
}

td::RefInt256 calculate_storage_fee(const td::optional<block::StoragePrices>& maybe_prices, bool is_masterchain,
                                    td::uint64 delta, td::uint64 bits, td::uint64 cells) {
  if (!maybe_prices) {
    // no StoragePrices is active, so the price is 0
    return td::zero_refint();
  }
  const block::StoragePrices& prices = maybe_prices.value();
  td::RefInt256 total;
  if (is_masterchain) {
    total = td::make_refint(cells) * prices.mc_cell_price;
    total += td::make_refint(bits) * prices.mc_bit_price;
  } else {
    total = td::make_refint(cells) * prices.cell_price;
    total += td::make_refint(bits) * prices.bit_price;
  }
  total *= delta;
  return td::rshift(total, 16, 1);
}

}  // namespace util

}  // namespace vm
