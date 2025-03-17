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
#include "TonlibClient.h"

#include "tonlib/ExtClientOutbound.h"
#include "tonlib/LastBlock.h"
#include "tonlib/LastConfig.h"
#include "tonlib/Logging.h"
#include "tonlib/utils.h"
#include "tonlib/keys/Mnemonic.h"
#include "tonlib/keys/SimpleEncryption.h"
#include "tonlib/TonlibError.h"

#include "smc-envelope/GenericAccount.h"
#include "smc-envelope/ManualDns.h"
#include "smc-envelope/WalletV3.h"
#include "smc-envelope/WalletV4.h"
#include "smc-envelope/HighloadWallet.h"
#include "smc-envelope/HighloadWalletV2.h"
#include "smc-envelope/PaymentChannel.h"
#include "smc-envelope/SmartContractCode.h"

#include "emulator/transaction-emulator.h"

#include "auto/tl/tonlib_api.hpp"
#include "block/block-auto.h"
#include "block/check-proof.h"
#include "ton/lite-tl.hpp"
#include "ton/ton-shard.h"
#include "lite-client/lite-client-common.h"

#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "vm/cp0.h"
#include "vm/memo.h"

#include "td/utils/as.h"
#include "td/utils/Random.h"
#include "td/utils/optional.h"
#include "td/utils/overloaded.h"

#include "td/utils/tests.h"
#include "td/utils/port/path.h"

#include "common/util.h"
#include "td/actor/MultiPromise.h"

template <class Type>
using lite_api_ptr = ton::lite_api::object_ptr<Type>;
template <class Type>
using tonlib_api_ptr = ton::tonlib_api::object_ptr<Type>;

namespace tonlib {
namespace int_api {
struct GetAccountState {
  block::StdAddress address;
  td::optional<ton::BlockIdExt> block_id;
  td::optional<td::Ed25519::PublicKey> public_key;
  using ReturnType = td::unique_ptr<AccountState>;
};

struct GetAccountStateByTransaction {
  block::StdAddress address;
  std::int64_t lt;
  td::Bits256 hash;
  //td::optional<td::Ed25519::PublicKey> public_key;
  using ReturnType = td::unique_ptr<AccountState>;
};

struct RemoteRunSmcMethod {
  block::StdAddress address;
  td::optional<ton::BlockIdExt> block_id;
  ton::SmartContract::Args args;
  bool need_result{false};

  using ReturnType = RemoteRunSmcMethodReturnType;
};

struct RemoteRunSmcMethodReturnType {
  ton::SmartContract::State smc_state;
  ton::BlockIdExt block_id;
  // result
  // c7
  // libs
};

struct ScanAndLoadGlobalLibs {
  td::Ref<vm::Cell> root;
  using ReturnType = vm::Dictionary;
};

struct GetPrivateKey {
  KeyStorage::InputKey input_key;
  using ReturnType = KeyStorage::PrivateKey;
};
struct GetDnsResolver {
  using ReturnType = block::StdAddress;
};
struct SendMessage {
  td::Ref<vm::Cell> message;
  using ReturnType = td::Unit;
};
}  // namespace int_api

template <class R, class O, class F>
R downcast_call2(O&& o, F&& f, R res = {}) {
  downcast_call(o, [&](auto& x) { res = f(x); });
  return res;
}

auto to_tonlib_api(const ton::BlockIdExt& blk) {
  return tonlib_api::make_object<tonlib_api::ton_blockIdExt>(
      blk.id.workchain, blk.id.shard, blk.id.seqno, blk.root_hash.as_slice().str(), blk.file_hash.as_slice().str());
}

tonlib_api::object_ptr<tonlib_api::options_configInfo> to_tonlib_api(const TonlibClient::FullConfig& full_config) {
  return tonlib_api::make_object<tonlib_api::options_configInfo>(full_config.wallet_id,
                                                                 full_config.rwallet_init_public_key);
}

class TonlibQueryActor : public td::actor::Actor {
 public:
  TonlibQueryActor(td::actor::ActorShared<TonlibClient> client) : client_(std::move(client)) {
  }
  template <class QueryT>
  void send_query(QueryT query, td::Promise<typename QueryT::ReturnType> promise) {
    td::actor::send_lambda(client_,
                           [self = client_.get(), query = std::move(query), promise = std::move(promise)]() mutable {
                             self.get_actor_unsafe().make_request(std::move(query), std::move(promise));
                           });
  }

 private:
  td::actor::ActorShared<TonlibClient> client_;
};

tonlib_api::object_ptr<tonlib_api::error> status_to_tonlib_api(const td::Status& status) {
  return tonlib_api::make_object<tonlib_api::error>(status.code(), status.message().str());
}

static block::AccountState create_account_state(ton::tl_object_ptr<ton::lite_api::liteServer_accountState> from) {
  block::AccountState res;
  res.blk = ton::create_block_id(from->id_);
  res.shard_blk = ton::create_block_id(from->shardblk_);
  res.shard_proof = std::move(from->shard_proof_);
  res.proof = std::move(from->proof_);
  res.state = std::move(from->state_);
  return res;
}
static block::AccountState create_account_state(ton::tl_object_ptr<ton::lite_api::liteServer_runMethodResult>& from) {
  block::AccountState res;
  res.blk = ton::create_block_id(from->id_);
  res.shard_blk = ton::create_block_id(from->shardblk_);
  res.shard_proof = std::move(from->shard_proof_);
  res.proof = std::move(from->proof_);
  res.state = std::move(from->state_proof_);
  res.is_virtualized = from->mode_ > 0;
  return res;
}
struct RawAccountState {
  td::int64 balance = -1;
  td::Ref<vm::Cell> extra_currencies;

  ton::UnixTime storage_last_paid{0};
  vm::CellStorageStat storage_stat;

  td::Ref<vm::Cell> code;
  td::Ref<vm::Cell> data;
  td::Ref<vm::Cell> state;
  std::string frozen_hash;
  block::AccountState::Info info;
  ton::BlockIdExt block_id;
};

tonlib_api::object_ptr<tonlib_api::internal_transactionId> empty_transaction_id() {
  return tonlib_api::make_object<tonlib_api::internal_transactionId>(0, std::string(32, 0));
}

tonlib_api::object_ptr<tonlib_api::internal_transactionId> to_transaction_id(const block::AccountState::Info& info) {
  return tonlib_api::make_object<tonlib_api::internal_transactionId>(info.last_trans_lt,
                                                                     info.last_trans_hash.as_slice().str());
}

std::string to_bytes(td::Ref<vm::Cell> cell) {
  if (cell.is_null()) {
    return "";
  }
  return vm::std_boc_serialize(cell, vm::BagOfCells::Mode::WithCRC32C).move_as_ok().as_slice().str();
}

td::Result<std::vector<tonlib_api::object_ptr<tonlib_api::extraCurrency>>> parse_extra_currencies_or_throw(
    const td::Ref<vm::Cell> dict_root) {
  std::vector<tonlib_api::object_ptr<tonlib_api::extraCurrency>> result;
  vm::Dictionary dict{dict_root, 32};
  if (!dict.check_for_each([&](td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int n) {
        CHECK(n == 32);
        int id = (int)key.get_int(n);
        auto amount_ref = block::tlb::t_VarUIntegerPos_32.as_integer_skip(value.write());
        if (amount_ref.is_null() || !value->empty_ext()) {
          return false;
        }
        td::int64 amount = amount_ref->to_long();
        if (amount == td::int64(~0ULL << 63)) {
          return false;
        }
        result.push_back(tonlib_api::make_object<tonlib_api::extraCurrency>(id, amount));
        return true;
      })) {
    return td::Status::Error("Failed to parse extra currencies dict");
  }
  return result;
}

td::Result<std::vector<tonlib_api::object_ptr<tonlib_api::extraCurrency>>> parse_extra_currencies(
    const td::Ref<vm::Cell>& dict_root) {
  return TRY_VM(parse_extra_currencies_or_throw(dict_root));
}

td::Result<td::Ref<vm::Cell>> to_extra_currenctes_dict(
    const std::vector<tonlib_api::object_ptr<tonlib_api::extraCurrency>>& extra_currencies) {
  vm::Dictionary dict{32};
  for (const auto &f : extra_currencies) {
    if (f->amount_ == 0) {
      continue;
    }
    if (f->amount_ < 0) {
      return td::Status::Error("Negative extra currency amount");
    }
    vm::CellBuilder cb2;
    block::tlb::t_VarUInteger_32.store_integer_value(cb2, *td::make_refint(f->amount_));
    if (!dict.set_builder(td::BitArray<32>(f->id_), cb2, vm::DictionaryBase::SetMode::Add)) {
      return td::Status::Error("Duplicate extra currency id");
    }
  }
  return std::move(dict).extract_root_cell();
}

td::Status check_enough_extra_currencies(const td::Ref<vm::Cell> &balance, const td::Ref<vm::Cell> &amount) {
  block::CurrencyCollection c1{td::zero_refint(), balance};
  block::CurrencyCollection c2{td::zero_refint(), amount};
  auto res = TRY_VM(td::Result<bool>{c1 >= c2});
  TRY_RESULT(v, std::move(res));
  if (!v) {
    return TonlibError::NotEnoughFunds();
  }
  return td::Status::OK();
}

td::Result<td::Ref<vm::Cell>> add_extra_currencies(const td::Ref<vm::Cell> &e1, const td::Ref<vm::Cell> &e2) {
  block::CurrencyCollection c1{td::zero_refint(), e1};
  block::CurrencyCollection c2{td::zero_refint(), e2};
  TRY_RESULT_ASSIGN(c1, TRY_VM(td::Result<block::CurrencyCollection>{c1 + c2}));
  if (c1.is_valid()) {
    return td::Status::Error("Failed to add extra currencies");
  }
  return c1.extra;
}

td::Result<block::PublicKey> get_public_key(td::Slice public_key) {
  TRY_RESULT_PREFIX(address, block::PublicKey::parse(public_key), TonlibError::InvalidPublicKey());
  return address;
}

td::Result<block::StdAddress> get_account_address(td::Slice account_address) {
  TRY_RESULT_PREFIX(address, block::StdAddress::parse(account_address), TonlibError::InvalidAccountAddress());
  return address;
}

td::Result<block::PublicKey> public_key_from_bytes(td::Slice bytes) {
  TRY_RESULT_PREFIX(key_bytes, block::PublicKey::from_bytes(bytes), TonlibError::Internal());
  return key_bytes;
}

td::Result<ton::WalletV3::InitData> to_init_data(const tonlib_api::wallet_v3_initialAccountState& wallet_state) {
  TRY_RESULT(key_bytes, get_public_key(wallet_state.public_key_));
  ton::WalletV3::InitData init_data;
  init_data.public_key = td::SecureString(key_bytes.key);
  init_data.wallet_id = static_cast<td::uint32>(wallet_state.wallet_id_);
  return std::move(init_data);
}

td::Result<ton::WalletV4::InitData> to_init_data(const tonlib_api::wallet_v4_initialAccountState& wallet_state) {
  TRY_RESULT(key_bytes, get_public_key(wallet_state.public_key_));
  ton::WalletV4::InitData init_data;
  init_data.public_key = td::SecureString(key_bytes.key);
  init_data.wallet_id = static_cast<td::uint32>(wallet_state.wallet_id_);
  return std::move(init_data);
}

td::Result<ton::RestrictedWallet::InitData> to_init_data(const tonlib_api::rwallet_initialAccountState& rwallet_state) {
  TRY_RESULT(init_key_bytes, get_public_key(rwallet_state.init_public_key_));
  TRY_RESULT(key_bytes, get_public_key(rwallet_state.public_key_));
  ton::RestrictedWallet::InitData init_data;
  init_data.init_key = td::SecureString(init_key_bytes.key);
  init_data.main_key = td::SecureString(key_bytes.key);
  init_data.wallet_id = static_cast<td::uint32>(rwallet_state.wallet_id_);
  return std::move(init_data);
}

td::Result<ton::pchan::Config> to_pchan_config(const tonlib_api::pchan_initialAccountState& pchan_state) {
  ton::pchan::Config config;
  if (!pchan_state.config_) {
    return TonlibError::EmptyField("config");
  }
  TRY_RESULT_PREFIX(a_key, get_public_key(pchan_state.config_->alice_public_key_),
                    TonlibError::InvalidField("alice_public_key", ""));
  config.a_key = td::SecureString(a_key.key);
  TRY_RESULT_PREFIX(b_key, get_public_key(pchan_state.config_->bob_public_key_),
                    TonlibError::InvalidField("bob_public_key", ""));
  config.b_key = td::SecureString(b_key.key);

  if (!pchan_state.config_->alice_address_) {
    return TonlibError::EmptyField("config.alice_address");
  }
  TRY_RESULT_PREFIX(a_addr, get_account_address(pchan_state.config_->alice_address_->account_address_),
                    TonlibError::InvalidField("alice_address", ""));
  config.a_addr = std::move(a_addr);

  if (!pchan_state.config_->bob_address_) {
    return TonlibError::EmptyField("config.bob_address");
  }
  TRY_RESULT_PREFIX(b_addr, get_account_address(pchan_state.config_->bob_address_->account_address_),
                    TonlibError::InvalidField("bob_address", ""));
  config.b_addr = std::move(b_addr);

  config.channel_id = pchan_state.config_->channel_id_;
  config.init_timeout = pchan_state.config_->init_timeout_;
  config.close_timeout = pchan_state.config_->close_timeout_;
  return std::move(config);
}

class AccountState {
 public:
  AccountState(block::StdAddress address, RawAccountState&& raw, td::uint32 wallet_id)
      : address_(std::move(address)), raw_(std::move(raw)), wallet_id_(wallet_id) {
    guess_type();
  }

  auto to_uninited_accountState() const {
    return tonlib_api::make_object<tonlib_api::uninited_accountState>(raw().frozen_hash);
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::raw_accountState>> to_raw_accountState() const {
    auto state = get_smc_state();
    std::string code;
    if (state.code.not_null()) {
      code = to_bytes(state.code);
    }
    std::string data;
    if (state.data.not_null()) {
      data = to_bytes(state.data);
    }
    return tonlib_api::make_object<tonlib_api::raw_accountState>(std::move(code), std::move(data), raw().frozen_hash);
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::raw_fullAccountState>> to_raw_fullAccountState() const {
    auto state = get_smc_state();
    std::string code;
    if (state.code.not_null()) {
      code = to_bytes(state.code);
    }
    std::string data;
    if (state.data.not_null()) {
      data = to_bytes(state.data);
    }
    TRY_RESULT(extra_currencies, parse_extra_currencies(get_extra_currencies()));
    return tonlib_api::make_object<tonlib_api::raw_fullAccountState>(
        get_balance(), std::move(extra_currencies), std::move(code), std::move(data), to_transaction_id(raw().info),
        to_tonlib_api(raw().block_id), raw().frozen_hash, get_sync_time());
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::wallet_v3_accountState>> to_wallet_v3_accountState() const {
    if (wallet_type_ != WalletV3) {
      return TonlibError::AccountTypeUnexpected("WalletV3");
    }
    auto wallet = ton::WalletV3(get_smc_state());
    TRY_RESULT(seqno, wallet.get_seqno());
    TRY_RESULT(wallet_id, wallet.get_wallet_id());
    return tonlib_api::make_object<tonlib_api::wallet_v3_accountState>(static_cast<td::uint32>(wallet_id),
                                                                       static_cast<td::uint32>(seqno));
  }
  td::Result<tonlib_api::object_ptr<tonlib_api::wallet_v4_accountState>> to_wallet_v4_accountState() const {
    if (wallet_type_ != WalletV4) {
      return TonlibError::AccountTypeUnexpected("WalletV4");
    }
    auto wallet = ton::WalletV4(get_smc_state());
    TRY_RESULT(seqno, wallet.get_seqno());
    TRY_RESULT(wallet_id, wallet.get_wallet_id());
    return tonlib_api::make_object<tonlib_api::wallet_v4_accountState>(static_cast<td::uint32>(wallet_id),
                                                                       static_cast<td::uint32>(seqno));
  }
  td::Result<tonlib_api::object_ptr<tonlib_api::wallet_highload_v1_accountState>> to_wallet_highload_v1_accountState()
      const {
    if (wallet_type_ != HighloadWalletV1) {
      return TonlibError::AccountTypeUnexpected("HighloadWalletV1");
    }
    auto wallet = ton::HighloadWallet(get_smc_state());
    TRY_RESULT(seqno, wallet.get_seqno());
    TRY_RESULT(wallet_id, wallet.get_wallet_id());
    return tonlib_api::make_object<tonlib_api::wallet_highload_v1_accountState>(static_cast<td::uint32>(wallet_id),
                                                                                static_cast<td::uint32>(seqno));
  }
  td::Result<tonlib_api::object_ptr<tonlib_api::wallet_highload_v2_accountState>> to_wallet_highload_v2_accountState()
      const {
    if (wallet_type_ != HighloadWalletV2) {
      return TonlibError::AccountTypeUnexpected("HighloadWalletV2");
    }
    auto wallet = ton::HighloadWalletV2(get_smc_state());
    TRY_RESULT(wallet_id, wallet.get_wallet_id());
    return tonlib_api::make_object<tonlib_api::wallet_highload_v2_accountState>(static_cast<td::uint32>(wallet_id));
  }
  td::Result<tonlib_api::object_ptr<tonlib_api::rwallet_accountState>> to_rwallet_accountState() const {
    if (wallet_type_ != RestrictedWallet) {
      return TonlibError::AccountTypeUnexpected("RestrictedWallet");
    }
    auto wallet = ton::RestrictedWallet::create(get_smc_state());
    TRY_RESULT(seqno, wallet->get_seqno());
    TRY_RESULT(wallet_id, wallet->get_wallet_id());
    TRY_RESULT(balance, wallet->get_balance(raw_.balance, raw_.info.gen_utime));
    TRY_RESULT(config, wallet->get_config());

    auto api_config = tonlib_api::make_object<tonlib_api::rwallet_config>();
    api_config->start_at_ = config.start_at;
    for (auto& limit : config.limits) {
      api_config->limits_.push_back(tonlib_api::make_object<tonlib_api::rwallet_limit>(limit.first, limit.second));
    }

    return tonlib_api::make_object<tonlib_api::rwallet_accountState>(wallet_id, seqno, balance, std::move(api_config));
  }
  td::Result<tonlib_api::object_ptr<tonlib_api::pchan_accountState>> to_payment_channel_accountState() const {
    if (wallet_type_ != PaymentChannel) {
      return TonlibError::AccountTypeUnexpected("PaymentChannel");
    }
    auto pchan = ton::PaymentChannel::create(get_smc_state());
    TRY_RESULT(info, pchan->get_info());
    TRY_RESULT(a_key, public_key_from_bytes(info.config.a_key));
    TRY_RESULT(b_key, public_key_from_bytes(info.config.b_key));

    tonlib_api::object_ptr<tonlib_api::pchan_State> tl_state;
    info.state.visit(td::overloaded(
        [&](const ton::pchan::StateInit& state) {
          tl_state = tonlib_api::make_object<tonlib_api::pchan_stateInit>(
              state.signed_A, state.signed_B, state.min_A, state.min_B, state.A, state.B, state.expire_at);
        },
        [&](const ton::pchan::StateClose& state) {
          tl_state = tonlib_api::make_object<tonlib_api::pchan_stateClose>(
              state.signed_A, state.signed_B, state.promise_A, state.promise_B, state.A, state.B, state.expire_at);
        },
        [&](const ton::pchan::StatePayout& state) {
          tl_state = tonlib_api::make_object<tonlib_api::pchan_statePayout>(state.A, state.B);
        }));

    using tonlib_api::make_object;
    return tonlib_api::make_object<tonlib_api::pchan_accountState>(
        tonlib_api::make_object<tonlib_api::pchan_config>(
            a_key.serialize(true), make_object<tonlib_api::accountAddress>(info.config.a_addr.rserialize(true)),
            b_key.serialize(true), make_object<tonlib_api::accountAddress>(info.config.b_addr.rserialize(true)),
            info.config.init_timeout, info.config.close_timeout, info.config.channel_id),
        std::move(tl_state), info.description);
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::dns_accountState>> to_dns_accountState() const {
    if (wallet_type_ != ManualDns) {
      return TonlibError::AccountTypeUnexpected("ManualDns");
    }
    TRY_RESULT(wallet_id, ton::ManualDns(get_smc_state()).get_wallet_id());
    return tonlib_api::make_object<tonlib_api::dns_accountState>(static_cast<td::uint32>(wallet_id));
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::AccountState>> to_accountState() const {
    auto f = [](auto&& r_x) -> td::Result<tonlib_api::object_ptr<tonlib_api::AccountState>> {
      TRY_RESULT(x, std::move(r_x));
      return std::move(x);
    };

    switch (wallet_type_) {
      case Empty:
        return to_uninited_accountState();
      case Unknown:
        return f(to_raw_accountState());
      case WalletV3:
        return f(to_wallet_v3_accountState());
      case HighloadWalletV1:
        return f(to_wallet_highload_v1_accountState());
      case HighloadWalletV2:
        return f(to_wallet_highload_v2_accountState());
      case RestrictedWallet:
        return f(to_rwallet_accountState());
      case ManualDns:
        return f(to_dns_accountState());
      case PaymentChannel:
        return f(to_payment_channel_accountState());
      case WalletV4:
        return f(to_wallet_v4_accountState());
    }
    UNREACHABLE();
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::fullAccountState>> to_fullAccountState() const {
    TRY_RESULT(account_state, to_accountState());
    TRY_RESULT(extra_currencies, parse_extra_currencies(get_extra_currencies()));
    return tonlib_api::make_object<tonlib_api::fullAccountState>(
        tonlib_api::make_object<tonlib_api::accountAddress>(get_address().rserialize(true)), get_balance(),
        std::move(extra_currencies), to_transaction_id(raw().info), to_tonlib_api(raw().block_id), get_sync_time(),
        std::move(account_state), get_wallet_revision());
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::tvm_cell>> to_shardAccountCell() const {
    auto account_root = raw_.info.root;
    if (account_root.is_null()) {
      block::gen::Account().cell_pack_account_none(account_root);
    }
    auto cell = vm::CellBuilder().store_ref(account_root).store_bits(raw_.info.last_trans_hash.as_bitslice()).store_long(raw_.info.last_trans_lt).finalize();
    return tonlib_api::make_object<tonlib_api::tvm_cell>(to_bytes(cell));
  }

  td::Result<td::Ref<vm::CellSlice>> to_shardAccountCellSlice() const {
    auto account_root = raw_.info.root;
    if (account_root.is_null()) {
      block::gen::Account().cell_pack_account_none(account_root);
    }
    return vm::CellBuilder().store_ref(account_root).store_bits(raw_.info.last_trans_hash.as_bitslice()).store_long(raw_.info.last_trans_lt).as_cellslice_ref();
  }

  //NB: Order is important! Used during guessAccountRevision
  enum WalletType {
    Empty,
    Unknown,
    WalletV3,
    HighloadWalletV1,
    HighloadWalletV2,
    ManualDns,
    PaymentChannel,
    RestrictedWallet,
    WalletV4
  };
  WalletType get_wallet_type() const {
    return wallet_type_;
  }
  td::int32 get_wallet_revision() const {
    return wallet_revision_;
  }
  bool is_wallet() const {
    switch (get_wallet_type()) {
      case AccountState::Empty:
      case AccountState::Unknown:
      case AccountState::ManualDns:
      case AccountState::PaymentChannel:
        return false;
      case AccountState::WalletV3:
      case AccountState::HighloadWalletV1:
      case AccountState::HighloadWalletV2:
      case AccountState::RestrictedWallet:
      case AccountState::WalletV4:
        return true;
    }
    UNREACHABLE();
    return false;
  }
  td::unique_ptr<ton::WalletInterface> get_wallet() const {
    switch (get_wallet_type()) {
      case AccountState::Empty:
      case AccountState::Unknown:
      case AccountState::ManualDns:
      case AccountState::PaymentChannel:
        return {};
      case AccountState::WalletV3:
        return td::make_unique<ton::WalletV3>(get_smc_state());
      case AccountState::HighloadWalletV1:
        return td::make_unique<ton::HighloadWallet>(get_smc_state());
      case AccountState::HighloadWalletV2:
        return td::make_unique<ton::HighloadWalletV2>(get_smc_state());
      case AccountState::RestrictedWallet:
        return td::make_unique<ton::RestrictedWallet>(get_smc_state());
      case AccountState::WalletV4:
        return td::make_unique<ton::WalletV4>(get_smc_state());
    }
    UNREACHABLE();
    return {};
  }
  bool is_frozen() const {
    return !raw_.frozen_hash.empty();
  }

  const block::StdAddress& get_address() const {
    return address_;
  }

  void make_non_bounceable() {
    address_.bounceable = false;
  }

  td::uint32 get_sync_time() const {
    return raw_.info.gen_utime;
  }

  ton::BlockIdExt get_block_id() const {
    return raw_.block_id;
  }

  td::int64 get_balance() const {
    return raw_.balance;
  }

  td::Ref<vm::Cell> get_extra_currencies() const {
    return raw_.extra_currencies;
  }

  const RawAccountState& raw() const {
    return raw_;
  }

  WalletType guess_type_by_init_state(tonlib_api::InitialAccountState& initial_account_state) {
    if (wallet_type_ != WalletType::Empty) {
      return wallet_type_;
    }
    downcast_call(
        initial_account_state,
        td::overloaded(
            [](auto& x) {},
            [&](tonlib_api::wallet_v3_initialAccountState& v3wallet) {
              for (auto revision : ton::SmartContractCode::get_revisions(ton::SmartContractCode::WalletV3)) {
                auto init_data = to_init_data(v3wallet);
                if (init_data.is_error()) {
                  continue;
                }
                auto wallet = ton::WalletV3::create(init_data.move_as_ok(), revision);
                if (!(wallet->get_address(ton::masterchainId) == address_ ||
                      wallet->get_address(ton::basechainId) == address_)) {
                  continue;
                }
                wallet_type_ = WalletType::WalletV3;
                wallet_revision_ = revision;
                set_new_state(wallet->get_state());
                break;
              }
            },
            [&](tonlib_api::wallet_v4_initialAccountState& v4wallet) {
              for (auto revision : ton::SmartContractCode::get_revisions(ton::SmartContractCode::WalletV4)) {
                auto init_data = to_init_data(v4wallet);
                if (init_data.is_error()) {
                  continue;
                }
                auto wallet = ton::WalletV4::create(init_data.move_as_ok(), revision);
                if (!(wallet->get_address(ton::masterchainId) == address_ ||
                      wallet->get_address(ton::basechainId) == address_)) {
                  continue;
                }
                wallet_type_ = WalletType::WalletV4;
                wallet_revision_ = revision;
                set_new_state(wallet->get_state());
                break;
              }
            },
            [&](tonlib_api::rwallet_initialAccountState& rwallet) {
              for (auto revision : ton::SmartContractCode::get_revisions(ton::SmartContractCode::RestrictedWallet)) {
                auto r_init_data = to_init_data(rwallet);
                if (r_init_data.is_error()) {
                  continue;
                }
                auto wallet = ton::RestrictedWallet::create(r_init_data.move_as_ok(), revision);
                if (!(wallet->get_address(ton::masterchainId) == address_ ||
                      wallet->get_address(ton::basechainId) == address_)) {
                  continue;
                }
                wallet_type_ = WalletType::RestrictedWallet;
                wallet_revision_ = revision;
                set_new_state(wallet->get_state());
                break;
              }
            },
            [&](tonlib_api::pchan_initialAccountState& pchan) {
              for (auto revision : ton::SmartContractCode::get_revisions(ton::SmartContractCode::PaymentChannel)) {
                auto r_conf = to_pchan_config(pchan);
                if (r_conf.is_error()) {
                  continue;
                }
                auto conf = r_conf.move_as_ok();
                auto wallet = ton::PaymentChannel::create(conf, revision);
                if (!(wallet->get_address(ton::masterchainId) == address_ ||
                      wallet->get_address(ton::basechainId) == address_)) {
                  continue;
                }
                wallet_type_ = WalletType::PaymentChannel;
                wallet_revision_ = revision;
                set_new_state(wallet->get_state());
                break;
              }
            }));
    return wallet_type_;
  }

  WalletType guess_type_by_public_key(td::Ed25519::PublicKey& key) {
    if (wallet_type_ != WalletType::Empty) {
      return wallet_type_;
    }
    auto wallet_id = static_cast<td::uint32>(address_.workchain + wallet_id_);
    ton::WalletInterface::DefaultInitData init_data{key.as_octet_string(), wallet_id};
    auto o_revision = ton::WalletV3::guess_revision(address_, init_data);
    if (o_revision) {
      wallet_type_ = WalletType::WalletV3;
      wallet_revision_ = o_revision.value();
      set_new_state(ton::WalletV3::get_init_state(wallet_revision_, init_data));
      return wallet_type_;
    }
    o_revision = ton::WalletV4::guess_revision(address_, init_data);
    if (o_revision) {
      wallet_type_ = WalletType::WalletV4;
      wallet_revision_ = o_revision.value();
      set_new_state(ton::WalletV4::get_init_state(wallet_revision_, init_data));
      return wallet_type_;
    }
    o_revision = ton::HighloadWalletV2::guess_revision(address_, init_data);
    if (o_revision) {
      wallet_type_ = WalletType::HighloadWalletV2;
      wallet_revision_ = o_revision.value();
      set_new_state(ton::HighloadWallet::get_init_state(wallet_revision_, init_data));
      return wallet_type_;
    }
    o_revision = ton::HighloadWallet::guess_revision(address_, init_data);
    if (o_revision) {
      wallet_type_ = WalletType::HighloadWalletV1;
      wallet_revision_ = o_revision.value();
      set_new_state(ton::HighloadWallet::get_init_state(wallet_revision_, init_data));
      return wallet_type_;
    }
    o_revision = ton::ManualDns::guess_revision(address_, key, wallet_id);
    if (o_revision) {
      wallet_type_ = WalletType::ManualDns;
      wallet_revision_ = o_revision.value();
      auto dns = ton::ManualDns::create(key, wallet_id, wallet_revision_);
      set_new_state(dns->get_state());
      return wallet_type_;
    }
    return wallet_type_;
  }

  WalletType guess_type_default(td::Ed25519::PublicKey& key) {
    if (wallet_type_ != WalletType::Empty) {
      return wallet_type_;
    }
    ton::WalletV3::InitData init_data(key.as_octet_string(), wallet_id_ + address_.workchain);
    set_new_state(ton::WalletV3::get_init_state(0, init_data));
    wallet_type_ = WalletType::WalletV3;
    return wallet_type_;
  }

  ton::SmartContract::State get_smc_state() const {
    return {raw_.code, raw_.data};
  }

  td::Ref<vm::Cell> get_raw_state() {
    return raw_.state;
  }

  void set_new_state(ton::SmartContract::State state) {
    raw_.code = std::move(state.code);
    raw_.data = std::move(state.data);
    raw_.state = ton::GenericAccount::get_init_state(raw_.code, raw_.data);
    has_new_state_ = true;
  }

  td::Ref<vm::Cell> get_new_state() const {
    if (!has_new_state_) {
      return {};
    }
    return raw_.state;
  }

 private:
  block::StdAddress address_;
  RawAccountState raw_;
  WalletType wallet_type_{Unknown};
  td::int32 wallet_revision_{0};
  td::uint32 wallet_id_{0};
  bool has_new_state_{false};

  WalletType guess_type() {
    if (raw_.code.is_null()) {
      wallet_type_ = WalletType::Empty;
      return wallet_type_;
    }
    auto code_hash = raw_.code->get_hash();
    auto o_revision = ton::WalletV3::guess_revision(code_hash);
    if (o_revision) {
      wallet_type_ = WalletType::WalletV3;
      wallet_revision_ = o_revision.value();
      return wallet_type_;
    }
    o_revision = ton::WalletV4::guess_revision(code_hash);
    if (o_revision) {
      wallet_type_ = WalletType::WalletV4;
      wallet_revision_ = o_revision.value();
      return wallet_type_;
    }
    o_revision = ton::HighloadWalletV2::guess_revision(code_hash);
    if (o_revision) {
      wallet_type_ = WalletType::HighloadWalletV2;
      wallet_revision_ = o_revision.value();
      return wallet_type_;
    }
    o_revision = ton::HighloadWallet::guess_revision(code_hash);
    if (o_revision) {
      wallet_type_ = WalletType::HighloadWalletV1;
      wallet_revision_ = o_revision.value();
      return wallet_type_;
    }
    o_revision = ton::ManualDns::guess_revision(code_hash);
    if (o_revision) {
      wallet_type_ = WalletType::ManualDns;
      wallet_revision_ = o_revision.value();
      return wallet_type_;
    }
    o_revision = ton::PaymentChannel::guess_revision(code_hash);
    if (o_revision) {
      wallet_type_ = WalletType::PaymentChannel;
      wallet_revision_ = o_revision.value();
      return wallet_type_;
    }
    o_revision = ton::RestrictedWallet::guess_revision(code_hash);
    if (o_revision) {
      wallet_type_ = WalletType::RestrictedWallet;
      wallet_revision_ = o_revision.value();
      return wallet_type_;
    }

    LOG(WARNING) << "Unknown code hash: " << td::base64_encode(code_hash.as_slice());
    wallet_type_ = WalletType::Unknown;
    return wallet_type_;
  }
};

class Query {
 public:
  struct Raw {
    td::unique_ptr<AccountState> source;
    std::vector<td::unique_ptr<AccountState>> destinations;

    td::uint32 valid_until{std::numeric_limits<td::uint32>::max()};

    td::Ref<vm::Cell> message;
    td::Ref<vm::Cell> new_state;
    td::Ref<vm::Cell> message_body;
  };

  Query(Raw&& raw) : raw_(std::move(raw)) {
  }

  td::Ref<vm::Cell> get_message() const {
    return raw_.message;
  }
  td::Ref<vm::Cell> get_message_body() const {
    return raw_.message_body;
  }
  td::Ref<vm::Cell> get_init_state() const {
    return raw_.new_state;
  }

  vm::CellHash get_body_hash() const {
    return raw_.message_body->get_hash();
  }

  td::uint32 get_valid_until() const {
    return raw_.valid_until;
  }

  // ported from block/transaction.cpp
  // TODO: reuse code
  static td::RefInt256 compute_threshold(const block::GasLimitsPrices& cfg) {
    auto gas_price256 = td::RefInt256{true, cfg.gas_price};
    if (cfg.gas_limit > cfg.flat_gas_limit) {
      return td::rshift(gas_price256 * (cfg.gas_limit - cfg.flat_gas_limit), 16, 1) +
             td::make_refint(cfg.flat_gas_price);
    } else {
      return td::make_refint(cfg.flat_gas_price);
    }
  }

  static td::uint64 gas_bought_for(td::RefInt256 nanograms, td::RefInt256 max_gas_threshold,
                                   const block::GasLimitsPrices& cfg) {
    if (nanograms.is_null() || sgn(nanograms) < 0) {
      return 0;
    }
    if (nanograms >= max_gas_threshold) {
      return cfg.gas_limit;
    }
    if (nanograms < cfg.flat_gas_price) {
      return 0;
    }
    auto gas_price256 = td::RefInt256{true, cfg.gas_price};
    auto res = td::div((std::move(nanograms) - cfg.flat_gas_price) << 16, gas_price256);
    return res->to_long() + cfg.flat_gas_limit;
  }

  static td::RefInt256 compute_gas_price(td::uint64 gas_used, const block::GasLimitsPrices& cfg) {
    auto gas_price256 = td::RefInt256{true, cfg.gas_price};
    return gas_used <= cfg.flat_gas_limit
               ? td::make_refint(cfg.flat_gas_price)
               : td::rshift(gas_price256 * (gas_used - cfg.flat_gas_limit), 16, 1) + cfg.flat_gas_price;
  }

  static vm::GasLimits compute_gas_limits(td::RefInt256 balance, const block::GasLimitsPrices& cfg) {
    vm::GasLimits res;
    // Compute gas limits
    if (false /*account.is_special*/) {
      res.gas_max = cfg.special_gas_limit;
    } else {
      res.gas_max = gas_bought_for(balance, compute_threshold(cfg), cfg);
    }
    res.gas_credit = 0;
    if (false /*trans_type != tr_ord*/) {
      // may use all gas that can be bought using remaining balance
      res.gas_limit = res.gas_max;
    } else {
      // originally use only gas bought using remaining message balance
      // if the message is "accepted" by the smart contract, the gas limit will be set to gas_max
      res.gas_limit = gas_bought_for(td::make_refint(0) /*msg balance remaining*/, compute_threshold(cfg), cfg);
      if (true /*!block::tlb::t_Message.is_internal(in_msg)*/) {
        // external messages carry no balance, give them some credit to check whether they are accepted
        res.gas_credit = std::min(static_cast<td::int64>(cfg.gas_credit), static_cast<td::int64>(res.gas_max));
      }
    }
    LOG(DEBUG) << "gas limits: max=" << res.gas_max << ", limit=" << res.gas_limit << ", credit=" << res.gas_credit;
    return res;
  }

  struct Fee {
    td::int64 in_fwd_fee{0};
    td::int64 storage_fee{0};
    td::int64 gas_fee{0};
    td::int64 fwd_fee{0};
    auto to_tonlib_api() const {
      return tonlib_api::make_object<tonlib_api::fees>(in_fwd_fee, storage_fee, gas_fee, fwd_fee);
    }
  };

  td::Result<td::int64> calc_fwd_fees(td::Ref<vm::Cell> list, block::MsgPrices** msg_prices, bool is_masterchain) {
    td::int64 res = 0;
    std::vector<td::Ref<vm::Cell>> actions;
    int n{0};
    int max_actions = 20;
    while (true) {
      actions.push_back(list);
      auto cs = load_cell_slice(std::move(list));
      if (!cs.size_ext()) {
        break;
      }
      if (!cs.have_refs()) {
        return td::Status::Error("action list invalid: entry found with data but no next reference");
      }
      list = cs.prefetch_ref();
      n++;
      if (n > max_actions) {
        return td::Status::Error(PSLICE() << "action list too long: more than " << max_actions << " actions");
      }
    }
    for (int i = n - 1; i >= 0; --i) {
      vm::CellSlice cs = load_cell_slice(actions[i]);
      CHECK(cs.fetch_ref().not_null());
      int tag = block::gen::t_OutAction.get_tag(cs);
      CHECK(tag >= 0);
      switch (tag) {
        case block::gen::OutAction::action_set_code:
          return td::Status::Error("estimate_fee: action_set_code unsupported");
        case block::gen::OutAction::action_send_msg: {
          block::gen::OutAction::Record_action_send_msg act_rec;
          // mode: +128 = attach all remaining balance, +64 = attach all remaining balance of the inbound message,
          // +1 = pay message fees, +2 = skip if message cannot be sent, +16 = bounce if action fails
          if (!tlb::unpack_exact(cs, act_rec) || (act_rec.mode & ~0xf3) || (act_rec.mode & 0xc0) == 0xc0) {
            return td::Status::Error("estimate_fee: can't parse send_msg");
          }
          block::gen::MessageRelaxed::Record msg;
          if (!tlb::type_unpack_cell(act_rec.out_msg, block::gen::t_MessageRelaxed_Any, msg)) {
            return td::Status::Error("estimate_fee: can't parse send_msg");
          }

          bool dest_is_masterchain = false;
          if (block::gen::t_CommonMsgInfoRelaxed.get_tag(*msg.info) == block::gen::CommonMsgInfoRelaxed::int_msg_info) {
            block::gen::CommonMsgInfoRelaxed::Record_int_msg_info info;
            if (!tlb::csr_unpack(msg.info, info)) {
              return td::Status::Error("estimate_fee: can't parse send_msg");
            }
            auto dest_addr = info.dest;
            if (!dest_addr->prefetch_ulong(1)) {
              return td::Status::Error("estimate_fee: messages with external addresses are unsupported");
            }
            int tag = block::gen::t_MsgAddressInt.get_tag(*dest_addr);

            if (tag == block::gen::MsgAddressInt::addr_std) {
              block::gen::MsgAddressInt::Record_addr_std recs;
              if (!tlb::csr_unpack(dest_addr, recs)) {
                return td::Status::Error("estimate_fee: can't parse send_msg");
              }
              dest_is_masterchain = recs.workchain_id == ton::masterchainId;
            }
          }
          vm::CellStorageStat sstat;                  // for message size
          sstat.add_used_storage(msg.init, true, 3);  // message init
          sstat.add_used_storage(msg.body, true, 3);  // message body (the root cell itself is not counted)
          res += msg_prices[is_masterchain || dest_is_masterchain]->compute_fwd_fees(sstat.cells, sstat.bits);
          break;
        }
        case block::gen::OutAction::action_reserve_currency:
          LOG(INFO) << "skip action_reserve_currency";
          continue;
      }
    }
    return res;
  }
  td::Result<std::pair<Fee, std::vector<Fee>>> estimate_fees(bool ignore_chksig, const LastConfigState& state,
                                                             vm::Dictionary& libraries) {
    // gas fees
    const auto& cfg = state.config;
    bool is_masterchain = raw_.source->get_address().workchain == ton::masterchainId;
    TRY_RESULT(gas_limits_prices, cfg->get_gas_limits_prices(is_masterchain));
    TRY_RESULT(storage_prices, cfg->get_storage_prices());
    TRY_RESULT(masterchain_msg_prices, cfg->get_msg_prices(true));
    TRY_RESULT(basechain_msg_prices, cfg->get_msg_prices(false));
    block::MsgPrices* msg_prices[2] = {&basechain_msg_prices, &masterchain_msg_prices};
    auto storage_fee_256 = block::StoragePrices::compute_storage_fees(
        raw_.source->get_sync_time(), storage_prices, raw_.source->raw().storage_stat,
        raw_.source->raw().storage_last_paid, false, is_masterchain);
    auto storage_fee = storage_fee_256.is_null() ? 0 : storage_fee_256->to_long();

    auto smc = ton::SmartContract::create(raw_.source->get_smc_state());

    td::int64 in_fwd_fee = 0;
    {
      vm::CellStorageStat sstat;                      // for message size
      sstat.add_used_storage(raw_.message, true, 3);  // message init
      in_fwd_fee += msg_prices[is_masterchain]->compute_fwd_fees(sstat.cells, sstat.bits);
    }

    vm::GasLimits gas_limits = compute_gas_limits(td::make_refint(raw_.source->get_balance()), gas_limits_prices);
    auto res = smc.write().send_external_message(raw_.message_body,
                                                 ton::SmartContract::Args()
                                                     .set_limits(gas_limits)
                                                     .set_balance(raw_.source->get_balance())
                                                     .set_extra_currencies(raw_.source->get_extra_currencies())
                                                     .set_now(raw_.source->get_sync_time())
                                                     .set_ignore_chksig(ignore_chksig)
                                                     .set_address(raw_.source->get_address())
                                                     .set_config(cfg)
                                                     .set_prev_blocks_info(state.prev_blocks_info)
                                                     .set_libraries(libraries));
    td::int64 fwd_fee = 0;
    if (res.success) {
      LOG(DEBUG) << "output actions:\n"
                 << block::gen::OutList{output_actions_count(res.actions)}.as_string_ref(res.actions);

      TRY_RESULT_ASSIGN(fwd_fee, calc_fwd_fees(res.actions, msg_prices, is_masterchain));
    }

    auto gas_fee = res.accepted ? compute_gas_price(res.gas_used, gas_limits_prices)->to_long() : 0;
    LOG(INFO) << storage_fee << " " << in_fwd_fee << " " << gas_fee << " " << fwd_fee << " " << res.gas_used;

    Fee fee;
    fee.in_fwd_fee = in_fwd_fee;
    fee.storage_fee = storage_fee;
    fee.gas_fee = gas_fee;
    fee.fwd_fee = fwd_fee;

    std::vector<Fee> dst_fees;

    for (auto& destination : raw_.destinations) {
      bool dest_is_masterchain = destination && destination->get_address().workchain == ton::masterchainId;
      TRY_RESULT(dest_gas_limits_prices, cfg->get_gas_limits_prices(dest_is_masterchain));
      auto dest_storage_fee_256 =
          destination ? block::StoragePrices::compute_storage_fees(
                            destination->get_sync_time(), storage_prices, destination->raw().storage_stat,
                            destination->raw().storage_last_paid, false, is_masterchain)
                      : td::make_refint(0);
      Fee dst_fee;
      auto dest_storage_fee = dest_storage_fee_256.is_null() ? 0 : dest_storage_fee_256->to_long();
      if (destination && destination->get_wallet_type() != AccountState::WalletType::Empty) {
        dst_fee.gas_fee = dest_gas_limits_prices.flat_gas_price;
        dst_fee.storage_fee = dest_storage_fee;
      }
      dst_fees.push_back(dst_fee);
    }
    return std::make_pair(fee, dst_fees);
  }

 private:
  Raw raw_;
  static int output_actions_count(td::Ref<vm::Cell> list) {
    int i = -1;
    do {
      ++i;
      list = load_cell_slice(std::move(list)).prefetch_ref();
    } while (list.not_null());
    return i;
  }
};

td::Result<td::int64> to_balance_or_throw(td::Ref<vm::CellSlice> balance_ref) {
  vm::CellSlice balance_slice = *balance_ref;
  auto balance = block::tlb::t_Grams.as_integer_skip(balance_slice);
  if (balance.is_null()) {
    return td::Status::Error("Failed to unpack balance");
  }
  auto res = balance->to_long();
  if (res == td::int64(~0ULL << 63)) {
    return td::Status::Error("Failed to unpack balance (2)");
  }
  return res;
}

td::Result<td::int64> to_balance(td::Ref<vm::CellSlice> balance_ref) {
  return TRY_VM(to_balance_or_throw(std::move(balance_ref)));
}

class GetTransactionHistory : public td::actor::Actor {
 public:
  GetTransactionHistory(ExtClientRef ext_client_ref, block::StdAddress address, ton::LogicalTime lt, ton::Bits256 hash, td::int32 count,
                        td::actor::ActorShared<> parent, td::Promise<block::TransactionList::Info> promise)
      : address_(std::move(address))
      , lt_(std::move(lt))
      , hash_(std::move(hash))
      , count_(count)
      , parent_(std::move(parent))
      , promise_(std::move(promise)) {
    client_.set_client(ext_client_ref);
  }

 private:
  block::StdAddress address_;
  ton::LogicalTime lt_;
  ton::Bits256 hash_;
  ExtClient client_;
  td::int32 count_;
  td::actor::ActorShared<> parent_;
  td::Promise<block::TransactionList::Info> promise_;

  void check(td::Status status) {
    if (status.is_error()) {
      promise_.set_error(std::move(status));
      stop();
    }
  }

  void with_transactions(
      td::Result<ton::lite_api::object_ptr<ton::lite_api::liteServer_transactionList>> r_transactions) {
    check(do_with_transactions(std::move(r_transactions)));
    stop();
  }

  td::Status do_with_transactions(
      td::Result<ton::lite_api::object_ptr<ton::lite_api::liteServer_transactionList>> r_transactions) {
    TRY_RESULT(transactions, std::move(r_transactions));
    TRY_RESULT_PREFIX(info, TRY_VM(do_with_transactions(std::move(transactions))), TonlibError::ValidateTransactions());
    promise_.set_value(std::move(info));
    return td::Status::OK();
  }

  td::Result<block::TransactionList::Info> do_with_transactions(
      ton::lite_api::object_ptr<ton::lite_api::liteServer_transactionList> transactions) {
    std::vector<ton::BlockIdExt> blkids;
    for (auto& id : transactions->ids_) {
      blkids.push_back(ton::create_block_id(std::move(id)));
    }
    return do_with_transactions(std::move(blkids), std::move(transactions->transactions_));
  }

  td::Result<block::TransactionList::Info> do_with_transactions(std::vector<ton::BlockIdExt> blkids,
                                                                td::BufferSlice transactions) {
    //LOG(INFO) << "got up to " << count_ << " transactions for " << address_ << " from last transaction " << lt_ << ":"
    //<< hash_.to_hex();
    block::TransactionList list;
    list.blkids = std::move(blkids);
    list.hash = hash_;
    list.lt = lt_;
    list.transactions_boc = std::move(transactions);
    TRY_RESULT(info, list.validate());
    if (info.transactions.size() > static_cast<size_t>(count_)) {
      LOG(WARNING) << "obtained " << info.transactions.size() << " transaction, but only " << count_
                   << " have been requested";
    }
    return info;
  }

  void start_up() override {
    if (lt_ == 0) {
      promise_.set_value(block::TransactionList::Info());
      stop();
      return;
    }
    client_.send_query(
        ton::lite_api::liteServer_getTransactions(
            count_, ton::create_tl_object<ton::lite_api::liteServer_accountId>(address_.workchain, address_.addr), lt_,
            hash_),
        [self = this](auto r_transactions) { self->with_transactions(std::move(r_transactions)); });
  }
  void hangup() override {
    check(TonlibError::Cancelled());
  }
};

class RemoteRunSmcMethod : public td::actor::Actor {
 public:
  RemoteRunSmcMethod(ExtClientRef ext_client_ref, int_api::RemoteRunSmcMethod query, td::actor::ActorShared<> parent,
                     td::Promise<int_api::RemoteRunSmcMethod::ReturnType>&& promise)
      : query_(std::move(query)), promise_(std::move(promise)), parent_(std::move(parent)) {
    client_.set_client(ext_client_ref);
  }

 private:
  int_api::RemoteRunSmcMethod query_;
  td::Promise<int_api::RemoteRunSmcMethod::ReturnType> promise_;
  td::actor::ActorShared<> parent_;
  ExtClient client_;

  void with_run_method_result(
      td::Result<ton::tl_object_ptr<ton::lite_api::liteServer_runMethodResult>> r_run_method_result) {
    check(do_with_run_method_result(std::move(r_run_method_result)));
  }

  td::Status do_with_run_method_result(
      td::Result<ton::tl_object_ptr<ton::lite_api::liteServer_runMethodResult>> r_run_method_result) {
    TRY_RESULT(run_method_result, std::move(r_run_method_result));
    TRY_RESULT_PREFIX(state, TRY_VM(do_with_run_method_result(std::move(run_method_result))),
                      TonlibError::ValidateAccountState());
    promise_.set_value(std::move(state));
    stop();
    return td::Status::OK();
  }
  td::Result<int_api::RemoteRunSmcMethod::ReturnType> do_with_run_method_result(
      ton::tl_object_ptr<ton::lite_api::liteServer_runMethodResult> run_method_result) {
    auto account_state = create_account_state(run_method_result);
    TRY_RESULT(info, account_state.validate(query_.block_id.value(), query_.address));
    auto serialized_state = account_state.state.clone();
    int_api::RemoteRunSmcMethod::ReturnType res;
    res.block_id = query_.block_id.value();
    auto cell = info.root;
    if (cell.is_null()) {
      return res;
    }
    block::gen::Account::Record_account account;
    if (!tlb::unpack_cell(cell, account)) {
      return td::Status::Error("Failed to unpack Account");
    }

    block::gen::AccountStorage::Record storage;
    if (!tlb::csr_unpack(account.storage, storage)) {
      return td::Status::Error("Failed to unpack AccountStorage");
    }
    auto state_tag = block::gen::t_AccountState.get_tag(*storage.state);
    if (state_tag < 0) {
      return td::Status::Error("Failed to parse AccountState tag");
    }
    if (state_tag != block::gen::AccountState::account_active) {
      return td::Status::Error("Account is not active");
    }
    block::gen::AccountState::Record_account_active state;
    if (!tlb::csr_unpack(storage.state, state)) {
      return td::Status::Error("Failed to parse AccountState");
    }
    block::gen::StateInit::Record state_init;
    if (!tlb::csr_unpack(state.x, state_init)) {
      return td::Status::Error("Failed to parse StateInit");
    }
    state_init.code->prefetch_maybe_ref(res.smc_state.code);
    state_init.data->prefetch_maybe_ref(res.smc_state.data);
    return res;
  }

  void with_last_block(td::Result<LastBlockState> r_last_block) {
    check(do_with_last_block(std::move(r_last_block)));
  }

  td::Status with_block_id() {
    TRY_RESULT(method_id, query_.args.get_method_id());
    TRY_RESULT(serialized_stack, query_.args.get_serialized_stack());
    client_.send_query(
        //liteServer.runSmcMethod mode:# id:tonNode.blockIdExt account:liteServer.accountId method_id:long params:bytes = liteServer.RunMethodResult;
        ton::lite_api::liteServer_runSmcMethod(
            0x17, ton::create_tl_lite_block_id(query_.block_id.value()),
            ton::create_tl_object<ton::lite_api::liteServer_accountId>(query_.address.workchain, query_.address.addr),
            method_id, std::move(serialized_stack)),
        [self = this](auto r_state) { self->with_run_method_result(std::move(r_state)); },
        query_.block_id.value().id.seqno);
    return td::Status::OK();
  }

  td::Status do_with_last_block(td::Result<LastBlockState> r_last_block) {
    TRY_RESULT(last_block, std::move(r_last_block));
    query_.block_id = std::move(last_block.last_block_id);
    return with_block_id();
  }

  void start_up() override {
    if (query_.block_id) {
      check(with_block_id());
    } else {
      client_.with_last_block(
          [self = this](td::Result<LastBlockState> r_last_block) { self->with_last_block(std::move(r_last_block)); });
    }
  }

  void check(td::Status status) {
    if (status.is_error()) {
      promise_.set_error(std::move(status));
      stop();
    }
  }
  void hangup() override {
    check(TonlibError::Cancelled());
  }
};

class GetRawAccountState : public td::actor::Actor {
 public:
  GetRawAccountState(ExtClientRef ext_client_ref, block::StdAddress address, td::optional<ton::BlockIdExt> block_id,
                     td::actor::ActorShared<> parent, td::Promise<RawAccountState>&& promise)
      : address_(std::move(address))
      , block_id_(std::move(block_id))
      , promise_(std::move(promise))
      , parent_(std::move(parent)) {
    client_.set_client(ext_client_ref);
  }

 private:
  block::StdAddress address_;
  td::optional<ton::BlockIdExt> block_id_;
  td::Promise<RawAccountState> promise_;
  td::actor::ActorShared<> parent_;
  ExtClient client_;

  void with_account_state(td::Result<ton::tl_object_ptr<ton::lite_api::liteServer_accountState>> r_account_state) {
    check(do_with_account_state(std::move(r_account_state)));
  }

  td::Status do_with_account_state(
      td::Result<ton::tl_object_ptr<ton::lite_api::liteServer_accountState>> r_raw_account_state) {
    TRY_RESULT(raw_account_state, std::move(r_raw_account_state));
    TRY_RESULT_PREFIX(state, TRY_VM(do_with_account_state(std::move(raw_account_state))),
                      TonlibError::ValidateAccountState());
    promise_.set_value(std::move(state));
    stop();
    return td::Status::OK();
  }

  td::Result<RawAccountState> do_with_account_state(
      ton::tl_object_ptr<ton::lite_api::liteServer_accountState> raw_account_state) {
    auto account_state = create_account_state(std::move(raw_account_state));
    TRY_RESULT(info, account_state.validate(block_id_.value(), address_));
    auto serialized_state = account_state.state.clone();
    RawAccountState res;
    res.block_id = block_id_.value();
    res.info = std::move(info);
    auto cell = res.info.root;
    //std::ostringstream outp;
    //block::gen::t_Account.print_ref(outp, cell);
    //LOG(INFO) << outp.str();
    if (cell.is_null()) {
      return res;
    }
    block::gen::Account::Record_account account;
    if (!tlb::unpack_cell(cell, account)) {
      return td::Status::Error("Failed to unpack Account");
    }
    {
      block::gen::StorageInfo::Record storage_info;
      if (!tlb::csr_unpack(account.storage_stat, storage_info)) {
        return td::Status::Error("Failed to unpack StorageInfo");
      }
      res.storage_last_paid = storage_info.last_paid;
      td::RefInt256 due_payment;
      if (storage_info.due_payment->prefetch_ulong(1) == 1) {
        vm::CellSlice& cs2 = storage_info.due_payment.write();
        cs2.advance(1);
        due_payment = block::tlb::t_Grams.as_integer_skip(cs2);
        if (due_payment.is_null() || !cs2.empty_ext()) {
          return td::Status::Error("Failed to upack due_payment");
        }
      } else {
        due_payment = td::RefInt256{true, 0};
      }
      block::gen::StorageUsed::Record storage_used;
      if (!tlb::csr_unpack(storage_info.used, storage_used)) {
        return td::Status::Error("Failed to unpack StorageInfo");
      }
      unsigned long long u = 0;
      vm::CellStorageStat storage_stat;
      u |= storage_stat.cells = block::tlb::t_VarUInteger_7.as_uint(*storage_used.cells);
      u |= storage_stat.bits = block::tlb::t_VarUInteger_7.as_uint(*storage_used.bits);
      u |= storage_stat.public_cells = block::tlb::t_VarUInteger_7.as_uint(*storage_used.public_cells);
      //LOG(DEBUG) << "last_paid=" << res.storage_last_paid << "; cells=" << storage_stat.cells
      //<< " bits=" << storage_stat.bits << " public_cells=" << storage_stat.public_cells;
      if (u == std::numeric_limits<td::uint64>::max()) {
        return td::Status::Error("Failed to unpack StorageStat");
      }

      res.storage_stat = storage_stat;
    }

    block::gen::AccountStorage::Record storage;
    if (!tlb::csr_unpack(account.storage, storage)) {
      return td::Status::Error("Failed to unpack AccountStorage");
    }
    TRY_RESULT(balance, to_balance(storage.balance));
    res.balance = balance;
    res.extra_currencies = storage.balance->prefetch_ref();
    auto state_tag = block::gen::t_AccountState.get_tag(*storage.state);
    if (state_tag < 0) {
      return td::Status::Error("Failed to parse AccountState tag");
    }
    if (state_tag == block::gen::AccountState::account_frozen) {
      block::gen::AccountState::Record_account_frozen state;
      if (!tlb::csr_unpack(storage.state, state)) {
        return td::Status::Error("Failed to parse AccountState");
      }
      res.frozen_hash = state.state_hash.as_slice().str();
      return res;
    }
    if (state_tag != block::gen::AccountState::account_active) {
      return res;
    }
    block::gen::AccountState::Record_account_active state;
    if (!tlb::csr_unpack(storage.state, state)) {
      return td::Status::Error("Failed to parse AccountState");
    }
    block::gen::StateInit::Record state_init;
    res.state = vm::CellBuilder().append_cellslice(state.x).finalize();
    if (!tlb::csr_unpack(state.x, state_init)) {
      return td::Status::Error("Failed to parse StateInit");
    }
    state_init.code->prefetch_maybe_ref(res.code);
    state_init.data->prefetch_maybe_ref(res.data);
    return res;
  }

  void with_last_block(td::Result<LastBlockState> r_last_block) {
    check(do_with_last_block(std::move(r_last_block)));
  }

  void with_block_id() {
    client_.send_query(
        ton::lite_api::liteServer_getAccountState(
            ton::create_tl_lite_block_id(block_id_.value()),
            ton::create_tl_object<ton::lite_api::liteServer_accountId>(address_.workchain, address_.addr)),
        [self = this](auto r_state) { self->with_account_state(std::move(r_state)); });
  }

  td::Status do_with_last_block(td::Result<LastBlockState> r_last_block) {
    TRY_RESULT(last_block, std::move(r_last_block));
    block_id_ = std::move(last_block.last_block_id);
    with_block_id();
    return td::Status::OK();
  }

  void start_up() override {
    if (block_id_) {
      with_block_id();
    } else {
      client_.with_last_block(
          [self = this](td::Result<LastBlockState> r_last_block) { self->with_last_block(std::move(r_last_block)); });
    }
  }

  void check(td::Status status) {
    if (status.is_error()) {
      promise_.set_error(std::move(status));
      stop();
    }
  }
  void hangup() override {
    check(TonlibError::Cancelled());
  }
};

class GetMasterchainBlockSignatures : public td::actor::Actor {
 public:
  GetMasterchainBlockSignatures(ExtClientRef ext_client_ref, ton::BlockSeqno seqno, td::actor::ActorShared<> parent,
                                td::Promise<tonlib_api_ptr<tonlib_api::blocks_blockSignatures>>&& promise)
      : block_id_short_(ton::masterchainId, ton::shardIdAll, seqno)
      , parent_(std::move(parent))
      , promise_(std::move(promise)) {
    client_.set_client(ext_client_ref);
  }

  void start_up() override {
    if (block_id_short_.seqno == 0) {
      abort(td::Status::Error("can't get signatures of block #0"));
      return;
    }
    client_.with_last_block([SelfId = actor_id(this)](td::Result<LastBlockState> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &GetMasterchainBlockSignatures::abort, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &GetMasterchainBlockSignatures::got_last_block, R.ok().last_block_id);
      }
    });
  }

  void got_last_block(ton::BlockIdExt id) {
    last_block_ = id;
    prev_block_id_short_ = block_id_short_;
    prev_block_id_short_.seqno--;
    client_.send_query(
        ton::lite_api::liteServer_lookupBlock(1, ton::create_tl_lite_block_id_simple(prev_block_id_short_), 0, 0),
        [SelfId = actor_id(this)](td::Result<lite_api_ptr<ton::lite_api::liteServer_blockHeader>> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &GetMasterchainBlockSignatures::abort, R.move_as_error());
          } else {
            td::actor::send_closure(SelfId, &GetMasterchainBlockSignatures::got_prev_block_id,
                                    ton::create_block_id(R.ok()->id_));
          }
        });
  }

  void got_prev_block_id(ton::BlockIdExt id) {
    prev_block_id_ = id;
    if (prev_block_id_.id != prev_block_id_short_) {
      abort(td::Status::Error("got incorrect block header from liteserver"));
      return;
    }
    client_.send_query(
        ton::lite_api::liteServer_getBlockProof(0x1001, ton::create_tl_lite_block_id(last_block_),
                                                ton::create_tl_lite_block_id(prev_block_id_)),
        [SelfId = actor_id(this)](td::Result<lite_api_ptr<ton::lite_api::liteServer_partialBlockProof>> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &GetMasterchainBlockSignatures::abort, R.move_as_error());
          } else {
            td::actor::send_closure(SelfId, &GetMasterchainBlockSignatures::got_prev_proof, R.move_as_ok());
          }
        });
  }

  void got_prev_proof(lite_api_ptr<ton::lite_api::liteServer_partialBlockProof> proof) {
    auto R = liteclient::deserialize_proof_chain(std::move(proof));
    if (R.is_error()) {
      abort(R.move_as_error());
      return;
    }
    auto chain = R.move_as_ok();
    if (chain->from != last_block_ || chain->to != prev_block_id_ || !chain->complete) {
      abort(td::Status::Error("got invalid proof chain"));
      return;
    }
    auto S = chain->validate();
    if (S.is_error()) {
      abort(std::move(S));
      return;
    }
    client_.send_query(
        ton::lite_api::liteServer_lookupBlock(1, ton::create_tl_lite_block_id_simple(block_id_short_), 0, 0),
        [SelfId = actor_id(this)](td::Result<lite_api_ptr<ton::lite_api::liteServer_blockHeader>> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &GetMasterchainBlockSignatures::abort, R.move_as_error());
          } else {
            td::actor::send_closure(SelfId, &GetMasterchainBlockSignatures::got_block_id,
                                    ton::create_block_id(R.ok()->id_));
          }
        });
  }

  void got_block_id(ton::BlockIdExt id) {
    block_id_ = id;
    client_.send_query(
        ton::lite_api::liteServer_getBlockProof(0x1001, ton::create_tl_lite_block_id(prev_block_id_),
                                                ton::create_tl_lite_block_id(block_id_)),
        [SelfId = actor_id(this)](td::Result<lite_api_ptr<ton::lite_api::liteServer_partialBlockProof>> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &GetMasterchainBlockSignatures::abort, R.move_as_error());
          } else {
            td::actor::send_closure(SelfId, &GetMasterchainBlockSignatures::got_proof, R.move_as_ok());
          }
        });
  }

  void got_proof(lite_api_ptr<ton::lite_api::liteServer_partialBlockProof> proof) {
    auto R = liteclient::deserialize_proof_chain(std::move(proof));
    if (R.is_error()) {
      abort(R.move_as_error());
      return;
    }
    auto chain = R.move_as_ok();
    if (chain->from != prev_block_id_ || chain->to != block_id_ || !chain->complete || chain->links.empty() ||
        chain->last_link().signatures.empty()) {
      abort(td::Status::Error("got invalid proof chain"));
      return;
    }
    auto S = chain->validate();
    if (S.is_error()) {
      abort(std::move(S));
      return;
    }
    std::vector<tonlib_api_ptr<tonlib_api::blocks_signature>> signatures;
    for (const auto& s : chain->last_link().signatures) {
      signatures.push_back(ton::create_tl_object<tonlib_api::blocks_signature>(s.node, s.signature.as_slice().str()));
    }
    promise_.set_result(
        ton::create_tl_object<tonlib_api::blocks_blockSignatures>(to_tonlib_api(block_id_), std::move(signatures)));
    stop();
  }

  void abort(td::Status error) {
    promise_.set_error(std::move(error));
    stop();
  }

 private:
  ton::BlockId block_id_short_;
  td::actor::ActorShared<> parent_;
  td::Promise<tonlib_api_ptr<tonlib_api::blocks_blockSignatures>> promise_;
  ExtClient client_;
  ton::BlockIdExt block_id_;
  ton::BlockId prev_block_id_short_;
  ton::BlockIdExt prev_block_id_;
  ton::BlockIdExt last_block_;
};

class GetShardBlockProof : public td::actor::Actor {
 public:
  GetShardBlockProof(ExtClientRef ext_client_ref, ton::BlockIdExt id, ton::BlockIdExt from,
                     td::actor::ActorShared<> parent,
                     td::Promise<tonlib_api_ptr<tonlib_api::blocks_shardBlockProof>>&& promise)
      : id_(id), from_(from), parent_(std::move(parent)), promise_(std::move(promise)) {
    client_.set_client(ext_client_ref);
  }

  void start_up() override {
    if (from_.is_masterchain_ext()) {
      got_from_block(from_);
    } else {
      client_.with_last_block([SelfId = actor_id(this)](td::Result<LastBlockState> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &GetShardBlockProof::abort, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &GetShardBlockProof::got_from_block, R.move_as_ok().last_block_id);
        }
      });
    }
  }

  void got_from_block(ton::BlockIdExt from) {
    from_ = from;
    CHECK(from_.is_masterchain_ext());
    client_.send_query(
        ton::lite_api::liteServer_getShardBlockProof(ton::create_tl_lite_block_id(id_)),
        [SelfId = actor_id(this)](td::Result<lite_api_ptr<ton::lite_api::liteServer_shardBlockProof>> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &GetShardBlockProof::abort, R.move_as_error());
          } else {
            td::actor::send_closure(SelfId, &GetShardBlockProof::got_shard_block_proof, R.move_as_ok());
          }
        });
  }

  void got_shard_block_proof(lite_api_ptr<ton::lite_api::liteServer_shardBlockProof> result) {
    mc_id_ = create_block_id(std::move(result->masterchain_id_));
    if (!mc_id_.is_masterchain_ext()) {
      abort(td::Status::Error("got invalid masterchain block id"));
      return;
    }
    if (result->links_.size() > 8) {
      abort(td::Status::Error("chain is too long"));
      return;
    }
    ton::BlockIdExt cur_id = mc_id_;
    try {
      for (auto& link : result->links_) {
        ton::BlockIdExt prev_id = create_block_id(link->id_);
        td::BufferSlice proof = std::move(link->proof_);
        auto R = vm::std_boc_deserialize(proof);
        if (R.is_error()) {
          abort(TonlibError::InvalidBagOfCells("proof"));
          return;
        }
        auto block_root = vm::MerkleProof::virtualize(R.move_as_ok(), 1);
        if (cur_id.root_hash != block_root->get_hash().bits()) {
          abort(td::Status::Error("invalid block hash in proof"));
          return;
        }
        if (cur_id.is_masterchain()) {
          block::gen::Block::Record blk;
          block::gen::BlockExtra::Record extra;
          block::gen::McBlockExtra::Record mc_extra;
          if (!tlb::unpack_cell(block_root, blk) || !tlb::unpack_cell(blk.extra, extra) || !extra.custom->have_refs() ||
              !tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra)) {
            abort(td::Status::Error("cannot unpack block header"));
            return;
          }
          block::ShardConfig shards(mc_extra.shard_hashes->prefetch_ref());
          td::Ref<block::McShardHash> shard_hash = shards.get_shard_hash(prev_id.shard_full(), true);
          if (shard_hash.is_null() || shard_hash->top_block_id() != prev_id) {
            abort(td::Status::Error("invalid proof chain: prev block is not in mc shard list"));
            return;
          }
        } else {
          std::vector<ton::BlockIdExt> prev;
          ton::BlockIdExt mc_blkid;
          bool after_split;
          td::Status S = block::unpack_block_prev_blk_try(block_root, cur_id, prev, mc_blkid, after_split);
          if (S.is_error()) {
            abort(std::move(S));
            return;
          }
          CHECK(prev.size() == 1 || prev.size() == 2);
          bool found = prev_id == prev[0] || (prev.size() == 2 && prev_id == prev[1]);
          if (!found) {
            abort(td::Status::Error("invalid proof chain: prev block is not in prev blocks list"));
            return;
          }
        }
        links_.emplace_back(prev_id, std::move(proof));
        cur_id = prev_id;
      }
    } catch (vm::VmVirtError& err) {
      abort(err.as_status());
      return;
    }
    if (cur_id != id_) {
      abort(td::Status::Error("got invalid proof chain"));
      return;
    }

    if (mc_id_.seqno() > from_.seqno()) {
      abort(td::Status::Error("from mc block is too old"));
      return;
    }

    client_.send_query(
        ton::lite_api::liteServer_getBlockProof(0x1001, ton::create_tl_lite_block_id(from_),
                                                ton::create_tl_lite_block_id(mc_id_)),
        [SelfId = actor_id(this)](td::Result<lite_api_ptr<ton::lite_api::liteServer_partialBlockProof>> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &GetShardBlockProof::abort, R.move_as_error());
          } else {
            td::actor::send_closure(SelfId, &GetShardBlockProof::got_mc_proof, R.move_as_ok());
          }
        });
  }

  void got_mc_proof(lite_api_ptr<ton::lite_api::liteServer_partialBlockProof> result) {
    auto R = liteclient::deserialize_proof_chain(std::move(result));
    if (R.is_error()) {
      abort(R.move_as_error());
      return;
    }
    auto chain = R.move_as_ok();
    if (chain->from != from_ || chain->to != mc_id_ || !chain->complete || chain->link_count() > 1) {
      abort(td::Status::Error("got invalid proof chain"));
      return;
    }
    auto S = chain->validate();
    if (S.is_error()) {
      abort(std::move(S));
      return;
    }

    std::vector<ton::tl_object_ptr<tonlib_api::blocks_shardBlockLink>> links;
    std::vector<ton::tl_object_ptr<tonlib_api::blocks_blockLinkBack>> mc_proof;
    for (const auto& p : links_) {
      links.push_back(
          ton::create_tl_object<tonlib_api::blocks_shardBlockLink>(to_tonlib_api(p.first), p.second.as_slice().str()));
    }
    if (chain->link_count() == 1) {
      auto& link = chain->last_link();
      td::BufferSlice dest_proof = vm::std_boc_serialize(link.dest_proof).move_as_ok();
      td::BufferSlice proof = vm::std_boc_serialize(link.proof).move_as_ok();
      td::BufferSlice state_proof = vm::std_boc_serialize(link.state_proof).move_as_ok();
      mc_proof.push_back(ton::create_tl_object<tonlib_api::blocks_blockLinkBack>(
          link.is_key, to_tonlib_api(link.from), to_tonlib_api(link.to), dest_proof.as_slice().str(),
          proof.as_slice().str(), state_proof.as_slice().str()));
    }

    promise_.set_result(ton::create_tl_object<tonlib_api::blocks_shardBlockProof>(
        to_tonlib_api(from_), to_tonlib_api(mc_id_), std::move(links), std::move(mc_proof)));
    stop();
  }

  void abort(td::Status error) {
    promise_.set_error(std::move(error));
    stop();
  }

 private:
  ton::BlockIdExt id_, from_, mc_id_;
  td::actor::ActorShared<> parent_;
  td::Promise<tonlib_api_ptr<tonlib_api::blocks_shardBlockProof>> promise_;
  ExtClient client_;
  std::vector<std::pair<ton::BlockIdExt, td::BufferSlice>> links_;
};

auto to_lite_api(const tonlib_api::ton_blockIdExt& blk) -> td::Result<lite_api_ptr<ton::lite_api::tonNode_blockIdExt>>;
auto to_tonlib_api(const ton::lite_api::liteServer_transactionId& txid) -> tonlib_api_ptr<tonlib_api::blocks_shortTxId>;

td::Status check_block_transactions_proof(lite_api_ptr<ton::lite_api::liteServer_blockTransactions>& bTxes, int32_t mode,
    ton::LogicalTime start_lt, td::Bits256 start_addr, td::Bits256 root_hash, int req_count);

class RunEmulator : public TonlibQueryActor {
 public:
  RunEmulator(ExtClientRef ext_client_ref, int_api::GetAccountStateByTransaction request,
      td::actor::ActorShared<TonlibClient> parent, td::Promise<td::unique_ptr<AccountState>>&& promise)
    : TonlibQueryActor(std::move(parent)), request_(std::move(request)), promise_(std::move(promise)) {
    client_.set_client(ext_client_ref);
  }

 private:
  struct FullBlockId {
    ton::BlockIdExt id;
    ton::BlockIdExt mc;
    ton::BlockIdExt prev;
    ton::Bits256 rand_seed;
  };

  ExtClient client_;
  int_api::GetAccountStateByTransaction request_;
  td::Promise<td::unique_ptr<AccountState>> promise_;

  std::map<td::int64, td::actor::ActorOwn<>> actors_;
  td::int64 actor_id_{1};

  FullBlockId block_id_;
  td::Ref<vm::Cell> mc_state_root_; // ^ShardStateUnsplit
  td::unique_ptr<AccountState> account_state_;
  vm::Dictionary global_libraries_{256};
  std::vector<td::Ref<vm::Cell>> transactions_; // std::vector<^Transaction>

  size_t count_{0};
  size_t count_transactions_{0};
  bool incomplete_{true};
  bool stopped_{false};

  void get_block_id(td::Promise<FullBlockId>&& promise) {
    auto shard_id = ton::shard_prefix(request_.address.addr, 60);
    auto query = ton::lite_api::liteServer_lookupBlock(0b111111010, ton::create_tl_lite_block_id_simple({request_.address.workchain, shard_id, 0}), request_.lt, 0);
    client_.send_query(std::move(query), promise.wrap([shard_id](td::Result<tonlib_api::object_ptr<ton::lite_api::liteServer_blockHeader>> header_r) -> td::Result<FullBlockId> {

      TRY_RESULT(header, std::move(header_r));
      ton::BlockIdExt block_id = ton::create_block_id(header->id_);
      TRY_RESULT(root, vm::std_boc_deserialize(std::move(header->header_proof_)));

      try {
        auto virt_root = vm::MerkleProof::virtualize(root, 1);
        if (virt_root.is_null()) {
          return td::Status::Error("block header proof is not a valid Merkle proof");
        }

        if (ton::RootHash{virt_root->get_hash().bits()} != block_id.root_hash) {
          return td::Status::Error("block header has incorrect root hash");
        }

        std::vector<ton::BlockIdExt> prev_blocks;
        ton::BlockIdExt mc_block_id;
        bool after_split;
        td::Status status = block::unpack_block_prev_blk_ext(virt_root, block_id, prev_blocks, mc_block_id, after_split);
        if (status.is_error()) {
          return status.move_as_error();
        }

        ton::BlockIdExt prev_block;
        if (prev_blocks.size() == 1 || ton::shard_is_ancestor(prev_blocks[0].id.shard, shard_id)) {
          prev_block = std::move(prev_blocks[0]);
        } else {
          prev_block = std::move(prev_blocks[1]);
        }

        block::gen::Block::Record block;
        block::gen::BlockExtra::Record extra;
        if (!tlb::unpack_cell(virt_root, block) || !tlb::unpack_cell(block.extra, extra)) {
          return td::Status::Error("cannot unpack block header");
        }

        return FullBlockId{std::move(block_id), std::move(mc_block_id), std::move(prev_block), std::move(extra.rand_seed)};
      } catch (vm::VmError& err) {
        return err.as_status("error processing header");
      } catch (vm::VmVirtError& err) {
        return err.as_status("error processing header");
      }
    }));
  }

  void get_mc_state_root(td::Promise<td::Ref<vm::Cell>>&& promise) {
    TRY_RESULT_PROMISE(promise, lite_block, to_lite_api(*to_tonlib_api(block_id_.mc)));
    auto block = ton::create_block_id(lite_block);
    client_.send_query(ton::lite_api::liteServer_getConfigAll(0b11'11111111, std::move(lite_block)), promise.wrap([block](auto r_config) -> td::Result<td::Ref<vm::Cell>> {

      TRY_RESULT(state, block::check_extract_state_proof(block, r_config->state_proof_.as_slice(), r_config->config_proof_.as_slice()));

      return std::move(state);
    }));
  }

  void get_account_state(td::Promise<td::unique_ptr<AccountState>>&& promise) {
    auto actor_id = actor_id_++;
    actors_[actor_id] = td::actor::create_actor<GetRawAccountState>(
      "GetAccountState", client_.get_client(), request_.address, block_id_.prev,
      actor_shared(this, actor_id),
      promise.wrap([address = request_.address](auto&& state) {
        return td::make_unique<AccountState>(std::move(address), std::move(state), 0);
    }));
  }

  td::Status get_transactions(std::int64_t lt) {
    TRY_RESULT(lite_block, to_lite_api(*to_tonlib_api(block_id_.id)));
    auto after = ton::lite_api::make_object<ton::lite_api::liteServer_transactionId3>(request_.address.addr, lt);
    auto mode = 0b10100111;
    constexpr int req_count = 256;
    auto query = ton::lite_api::liteServer_listBlockTransactions(std::move(lite_block), mode, req_count, std::move(after), false, true);

    client_.send_query(std::move(query), [self = this, mode, lt, root_hash = block_id_.id.root_hash](lite_api_ptr<ton::lite_api::liteServer_blockTransactions>&& bTxes) {
      if (!bTxes) {
        self->check(td::Status::Error("liteServer.blockTransactions is null"));
        return;
      }

      self->check(check_block_transactions_proof(bTxes, mode, lt, self->request_.address.addr, root_hash, req_count));

      std::int64_t last_lt = 0;
      for (auto& id : bTxes->ids_) {
        last_lt = id->lt_;
        if (id->account_ != self->request_.address.addr) {
          continue;
        }

        if (id->lt_ == self->request_.lt && id->hash_ == self->request_.hash) {
          self->incomplete_ = false;
        }

        self->transactions_.push_back({});
        self->get_transaction(id->lt_, id->hash_, [self, i = self->transactions_.size() - 1](auto transaction) { self->set_transaction(i, std::move(transaction)); });

        if (!self->incomplete_) {
          return;
        }
      }

      if (bTxes->incomplete_) {
        self->check(self->get_transactions(last_lt));
      } else {
        self->check(td::Status::Error("Transaction not found"));
      }
    });
    return td::Status::OK();
  }

  void get_transaction(std::int64_t lt, td::Bits256 hash, td::Promise<td::Ref<vm::Cell>>&& promise) {
    auto actor_id = actor_id_++;
    actors_[actor_id] = td::actor::create_actor<GetTransactionHistory>(
        "GetTransactionHistory", client_.get_client(), request_.address, lt, hash, 1, actor_shared(this, actor_id),
        promise.wrap([](auto&& transactions) mutable {
          return std::move(transactions.transactions.front().transaction);
        }));
  }

  void start_up() override {
    if (stopped_) {
      return;
    }
    get_block_id([SelfId = actor_id(this)](td::Result<FullBlockId>&& block_id) {
      td::actor::send_closure(SelfId, &RunEmulator::set_block_id, std::move(block_id));
    });
  }

  void set_block_id(td::Result<FullBlockId>&& block_id) {
    if (block_id.is_error()) {
      check(block_id.move_as_error());
    } else {
      block_id_ = block_id.move_as_ok();

      get_mc_state_root([SelfId = actor_id(this)](td::Result<td::Ref<vm::Cell>>&& mc_state_root) {
        td::actor::send_closure(SelfId, &RunEmulator::set_mc_state_root, std::move(mc_state_root));
      });
      get_account_state([SelfId = actor_id(this)](td::Result<td::unique_ptr<AccountState>>&& state) {
        td::actor::send_closure(SelfId, &RunEmulator::set_account_state, std::move(state));
      });
      check(get_transactions(0));

      inc();
    }
  }

  void set_mc_state_root(td::Result<td::Ref<vm::Cell>>&& mc_state_root) {
    if (mc_state_root.is_error()) {
      check(mc_state_root.move_as_error());
    } else {
      mc_state_root_ = mc_state_root.move_as_ok();
      inc();
    }
  }

  void set_account_state(td::Result<td::unique_ptr<AccountState>>&& account_state) {
    if (account_state.is_error()) {
      check(account_state.move_as_error());
    } else {
      account_state_ = account_state.move_as_ok();
      send_query(int_api::ScanAndLoadGlobalLibs{account_state_->get_raw_state()},
                 [SelfId = actor_id(this)](td::Result<vm::Dictionary> R) {
                   td::actor::send_closure(SelfId, &RunEmulator::set_global_libraries, std::move(R));
                 });
    }
  }

  void set_global_libraries(td::Result<vm::Dictionary> R) {
    if (R.is_error()) {
      check(R.move_as_error());
    } else {
      global_libraries_ = R.move_as_ok();
      inc();
    }
  }

  void set_transaction(size_t i, td::Result<td::Ref<vm::Cell>>&& transaction) {
    if (transaction.is_error()) {
      check(transaction.move_as_error());
    } else {
      transactions_[i] = transaction.move_as_ok();
      inc_transactions();
    }
  }

  void inc_transactions() {
    if (stopped_ || ++count_transactions_ != transactions_.size() || incomplete_) {
      return;
    }
    inc();
  }

  void inc() {
    if (stopped_ || ++count_ != 4) {  // 4 -- block_id + mc_state_root + account_state + transactions
      return;
    }

    try {
      auto r_config = block::ConfigInfo::extract_config(mc_state_root_, 0b11'11111111);
      if (r_config.is_error()) {
        check(r_config.move_as_error());
        return;
      }
      std::shared_ptr<block::ConfigInfo> config = r_config.move_as_ok();

      auto r_shard_account = account_state_->to_shardAccountCellSlice();
      if (r_shard_account.is_error()) {
        check(r_shard_account.move_as_error());
        return;
      }
      td::Ref<vm::CellSlice> shard_account = r_shard_account.move_as_ok();

      const block::StdAddress& address = account_state_->get_address();
      ton::UnixTime now = account_state_->get_sync_time();
      bool is_special = address.workchain == ton::masterchainId && config->is_special_smartcontract(address.addr);
      block::Account account(address.workchain, address.addr.bits());
      if (!account.unpack(std::move(shard_account), now, is_special)) {
        check(td::Status::Error("Can't unpack shard account"));
        return;
      }

      auto prev_blocks_info = config->get_prev_blocks_info();
      if (prev_blocks_info.is_error()) {
        check(prev_blocks_info.move_as_error());
        return;
      }
      vm::Dictionary libraries = global_libraries_;
      emulator::TransactionEmulator trans_emulator(config);
      trans_emulator.set_prev_blocks_info(prev_blocks_info.move_as_ok());
      trans_emulator.set_libs(std::move(libraries));
      trans_emulator.set_rand_seed(block_id_.rand_seed);
      td::Result<emulator::TransactionEmulator::EmulationChain> emulation_result =
          trans_emulator.emulate_transactions_chain(std::move(account), std::move(transactions_));

      if (emulation_result.is_error()) {
        promise_.set_error(emulation_result.move_as_error());
      } else {
        account = std::move(emulation_result.move_as_ok().account);
        RawAccountState raw = std::move(account_state_->raw());
        raw.block_id = block_id_.id;
        block::CurrencyCollection balance = account.get_balance();
        raw.balance = balance.grams->to_long();
        raw.extra_currencies = balance.extra;
        raw.storage_last_paid = std::move(account.last_paid);
        raw.storage_stat = std::move(account.storage_stat);
        raw.code = std::move(account.code);
        raw.data = std::move(account.data);
        raw.state = std::move(account.total_state);
        raw.info.last_trans_lt = account.last_trans_lt_;
        raw.info.last_trans_hash = account.last_trans_hash_;
        raw.info.gen_utime = account.now_;

        if (account.status == block::Account::acc_frozen) {
          raw.frozen_hash = (char*)account.state_hash.data();
        }

        promise_.set_value(td::make_unique<AccountState>(address, std::move(raw), 0));
      }
    } catch (vm::VmVirtError& err) {
      check(td::Status::Error(PSLICE() << "virtualization error while emulating transaction: " << err.get_msg()));
      return;
    }
    stopped_ = true;
    try_stop();
  }

  void check(td::Status status) {
    if (status.is_error()) {
      promise_.set_error(std::move(status));
      stopped_ = true;
      try_stop();
    }
  }

  void try_stop() {
    if (stopped_ && actors_.empty()) {
      stop();
    }
  }

  void hangup_shared() override {
    actors_.erase(get_link_token());
    try_stop();
  }

  void hangup() override {
    check(TonlibError::Cancelled());
  }
};

TonlibClient::TonlibClient(td::unique_ptr<TonlibCallback> callback) : callback_(std::move(callback)) {
}
TonlibClient::~TonlibClient() = default;

void TonlibClient::hangup() {
  source_.cancel();
  is_closing_ = true;
  ref_cnt_--;
  raw_client_ = {};
  raw_last_block_ = {};
  raw_last_config_ = {};
  try_stop();
}

ExtClientRef TonlibClient::get_client_ref() {
  ExtClientRef ref;
  ref.adnl_ext_client_ = raw_client_.get();
  ref.last_block_actor_ = raw_last_block_.get();
  ref.last_config_actor_ = raw_last_config_.get();

  return ref;
}

void TonlibClient::proxy_request(td::int64 query_id, std::string data) {
  on_update(tonlib_api::make_object<tonlib_api::updateSendLiteServerQuery>(query_id, data));
}

void TonlibClient::init_ext_client() {
  if (use_callbacks_for_network_) {
    class Callback : public ExtClientOutbound::Callback {
     public:
      explicit Callback(td::actor::ActorShared<TonlibClient> parent, td::uint32 config_generation)
          : parent_(std::move(parent)), config_generation_(config_generation) {
      }

      void request(td::int64 id, std::string data) override {
        send_closure(parent_, &TonlibClient::proxy_request, (id << 16) | (config_generation_ & 0xffff),
                     std::move(data));
      }

     private:
      td::actor::ActorShared<TonlibClient> parent_;
      td::uint32 config_generation_;
    };
    ref_cnt_++;
    auto client =
        ExtClientOutbound::create(td::make_unique<Callback>(td::actor::actor_shared(this), config_generation_));
    ext_client_outbound_ = client.get();
    raw_client_ = std::move(client);
  } else {
    ext_client_outbound_ = {};
    raw_client_ = liteclient::ExtClient::create(config_.lite_servers, nullptr);
  }
}

void TonlibClient::update_last_block_state(LastBlockState state, td::uint32 config_generation) {
  if (config_generation != config_generation_) {
    return;
  }

  last_block_storage_.save_state(last_state_key_, state);
}

void TonlibClient::update_sync_state(LastBlockSyncState state, td::uint32 config_generation) {
  if (config_generation != config_generation_) {
    return;
  }
  switch (state.type) {
    case LastBlockSyncState::Done:
      on_update(
          tonlib_api::make_object<tonlib_api::updateSyncState>(tonlib_api::make_object<tonlib_api::syncStateDone>()));
      break;
    case LastBlockSyncState::InProgress:
      on_update(
          tonlib_api::make_object<tonlib_api::updateSyncState>(tonlib_api::make_object<tonlib_api::syncStateInProgress>(
              state.from_seqno, state.to_seqno, state.current_seqno)));
      break;
    default:
      LOG(ERROR) << "Unknown LastBlockSyncState type " << state.type;
  }
}

void TonlibClient::init_last_block(LastBlockState state) {
  ref_cnt_++;
  class Callback : public LastBlock::Callback {
   public:
    Callback(td::actor::ActorShared<TonlibClient> client, td::uint32 config_generation)
        : client_(std::move(client)), config_generation_(config_generation) {
    }
    void on_state_changed(LastBlockState state) override {
      send_closure(client_, &TonlibClient::update_last_block_state, std::move(state), config_generation_);
    }
    void on_sync_state_changed(LastBlockSyncState sync_state) override {
      send_closure(client_, &TonlibClient::update_sync_state, std::move(sync_state), config_generation_);
    }

   private:
    td::actor::ActorShared<TonlibClient> client_;
    td::uint32 config_generation_;
  };

  last_block_storage_.save_state(last_state_key_, state);

  raw_last_block_ = td::actor::create_actor<LastBlock>(
      td::actor::ActorOptions().with_name("LastBlock").with_poll(false), get_client_ref(), std::move(state), config_,
      source_.get_cancellation_token(), td::make_unique<Callback>(td::actor::actor_shared(this), config_generation_));
}
void TonlibClient::init_last_config() {
  ref_cnt_++;
  class Callback : public LastConfig::Callback {
   public:
    Callback(td::actor::ActorShared<TonlibClient> client) : client_(std::move(client)) {
    }

   private:
    td::actor::ActorShared<TonlibClient> client_;
  };
  raw_last_config_ =
      td::actor::create_actor<LastConfig>(td::actor::ActorOptions().with_name("LastConfig").with_poll(false),
                                          get_client_ref(), td::make_unique<Callback>(td::actor::actor_shared(this)));
}

void TonlibClient::on_result(td::uint64 id, tonlib_api::object_ptr<tonlib_api::Object> response) {
  VLOG_IF(tonlib_query, id != 0) << "Tonlib answer query " << td::tag("id", id) << " " << to_string(response);
  VLOG_IF(tonlib_query, id == 0) << "Tonlib update " << to_string(response);
  if (response->get_id() == tonlib_api::error::ID) {
    callback_->on_error(id, tonlib_api::move_object_as<tonlib_api::error>(response));
    return;
  }
  callback_->on_result(id, std::move(response));
}
void TonlibClient::on_update(object_ptr<tonlib_api::Object> response) {
  on_result(0, std::move(response));
}

void TonlibClient::make_any_request(tonlib_api::Function& function, QueryContext query_context,
                                    td::Promise<tonlib_api::object_ptr<tonlib_api::Object>>&& promise) {
  auto old_context = std::move(query_context_);
  SCOPE_EXIT {
    query_context_ = std::move(old_context);
  };
  query_context_ = std::move(query_context);
  downcast_call(function, [&](auto& request) { this->make_request(request, promise.wrap([](auto x) { return x; })); });
}

void TonlibClient::request(td::uint64 id, tonlib_api::object_ptr<tonlib_api::Function> function) {
  VLOG(tonlib_query) << "Tonlib got query " << td::tag("id", id) << " " << to_string(function);
  if (function == nullptr) {
    LOG(ERROR) << "Receive empty static request";
    return on_result(id, tonlib_api::make_object<tonlib_api::error>(400, "Request is empty"));
  }

  if (is_static_request(function->get_id())) {
    return on_result(id, static_request(std::move(function)));
  }

  if (state_ == State::Closed) {
    return on_result(id, tonlib_api::make_object<tonlib_api::error>(400, "tonlib is closed"));
  }
  if (state_ == State::Uninited) {
    if (!is_uninited_request(function->get_id())) {
      return on_result(id, tonlib_api::make_object<tonlib_api::error>(400, "library is not inited"));
    }
  }

  ref_cnt_++;
  using Object = tonlib_api::object_ptr<tonlib_api::Object>;
  td::Promise<Object> promise = [actor_id = actor_id(this), id, tmp = actor_shared(this)](td::Result<Object> r_result) {
    tonlib_api::object_ptr<tonlib_api::Object> result;
    if (r_result.is_error()) {
      result = status_to_tonlib_api(r_result.error());
    } else {
      result = r_result.move_as_ok();
    }

    send_closure(actor_id, &TonlibClient::on_result, id, std::move(result));
  };

  make_any_request(*function, {}, std::move(promise));
}
void TonlibClient::close() {
  stop();
}
tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::static_request(
    tonlib_api::object_ptr<tonlib_api::Function> function) {
  VLOG(tonlib_query) << "Tonlib got static query " << to_string(function);
  if (function == nullptr) {
    LOG(ERROR) << "Receive empty static request";
    return tonlib_api::make_object<tonlib_api::error>(400, "Request is empty");
  }

  auto response = downcast_call2<tonlib_api::object_ptr<tonlib_api::Object>>(
      *function, [](auto& request) { return TonlibClient::do_static_request(request); });
  VLOG(tonlib_query) << "  answer static query " << to_string(response);
  return response;
}

bool TonlibClient::is_static_request(td::int32 id) {
  switch (id) {
    case tonlib_api::runTests::ID:
    case tonlib_api::getAccountAddress::ID:
    case tonlib_api::packAccountAddress::ID:
    case tonlib_api::unpackAccountAddress::ID:
    case tonlib_api::getBip39Hints::ID:
    case tonlib_api::setLogStream::ID:
    case tonlib_api::getLogStream::ID:
    case tonlib_api::setLogVerbosityLevel::ID:
    case tonlib_api::getLogVerbosityLevel::ID:
    case tonlib_api::getLogTags::ID:
    case tonlib_api::setLogTagVerbosityLevel::ID:
    case tonlib_api::getLogTagVerbosityLevel::ID:
    case tonlib_api::addLogMessage::ID:
    case tonlib_api::encrypt::ID:
    case tonlib_api::decrypt::ID:
    case tonlib_api::kdf::ID:
    case tonlib_api::msg_decryptWithProof::ID:
      return true;
    default:
      return false;
  }
}
bool TonlibClient::is_uninited_request(td::int32 id) {
  switch (id) {
    case tonlib_api::init::ID:
    case tonlib_api::close::ID:
      return true;
    default:
      return false;
  }
}

tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(const tonlib_api::runTests& request) {
  auto& runner = td::TestsRunner::get_default();
  if (!request.dir_.empty()) {
    td::chdir(request.dir_).ignore();
  }
  runner.run_all();
  return tonlib_api::make_object<tonlib_api::ok>();
}

td::Result<block::StdAddress> get_account_address(const tonlib_api::raw_initialAccountState& raw_state,
                                                  td::int32 revision, ton::WorkchainId workchain_id) {
  TRY_RESULT_PREFIX(code, vm::std_boc_deserialize(raw_state.code_), TonlibError::InvalidBagOfCells("raw_state.code"));
  TRY_RESULT_PREFIX(data, vm::std_boc_deserialize(raw_state.data_), TonlibError::InvalidBagOfCells("raw_state.data"));
  return ton::GenericAccount::get_address(workchain_id,
                                          ton::GenericAccount::get_init_state(std::move(code), std::move(data)));
}

td::Result<block::StdAddress> get_account_address(const tonlib_api::wallet_v3_initialAccountState& test_wallet_state,
                                                  td::int32 revision, ton::WorkchainId workchain_id) {
  TRY_RESULT(key_bytes, get_public_key(test_wallet_state.public_key_));
  return ton::WalletV3::create({key_bytes.key, static_cast<td::uint32>(test_wallet_state.wallet_id_)}, revision)
      ->get_address(workchain_id);
}

td::Result<block::StdAddress> get_account_address(const tonlib_api::wallet_v4_initialAccountState& test_wallet_state,
                                                  td::int32 revision, ton::WorkchainId workchain_id) {
  TRY_RESULT(key_bytes, get_public_key(test_wallet_state.public_key_));
  return ton::WalletV4::create({key_bytes.key, static_cast<td::uint32>(test_wallet_state.wallet_id_)}, revision)
      ->get_address(workchain_id);
}

td::Result<block::StdAddress> get_account_address(
    const tonlib_api::wallet_highload_v1_initialAccountState& test_wallet_state, td::int32 revision,
    ton::WorkchainId workchain_id) {
  TRY_RESULT(key_bytes, get_public_key(test_wallet_state.public_key_));
  return ton::HighloadWallet::create({key_bytes.key, static_cast<td::uint32>(test_wallet_state.wallet_id_)}, revision)
      ->get_address(workchain_id);
}

td::Result<block::StdAddress> get_account_address(
    const tonlib_api::wallet_highload_v2_initialAccountState& test_wallet_state, td::int32 revision,
    ton::WorkchainId workchain_id) {
  TRY_RESULT(key_bytes, get_public_key(test_wallet_state.public_key_));
  return ton::HighloadWalletV2::create({key_bytes.key, static_cast<td::uint32>(test_wallet_state.wallet_id_)}, revision)
      ->get_address(workchain_id);
}

td::Result<block::StdAddress> get_account_address(const tonlib_api::dns_initialAccountState& dns_state,
                                                  td::int32 revision, ton::WorkchainId workchain_id) {
  TRY_RESULT(key_bytes, get_public_key(dns_state.public_key_));
  auto key = td::Ed25519::PublicKey(td::SecureString(key_bytes.key));
  return ton::ManualDns::create(key, static_cast<td::uint32>(dns_state.wallet_id_), revision)
      ->get_address(workchain_id);
}

td::Result<block::StdAddress> get_account_address(const tonlib_api::pchan_initialAccountState& pchan_state,
                                                  td::int32 revision, ton::WorkchainId workchain_id) {
  TRY_RESULT(config, to_pchan_config(pchan_state));
  return ton::PaymentChannel::create(config, revision)->get_address(workchain_id);
}
td::Result<block::StdAddress> get_account_address(const tonlib_api::rwallet_initialAccountState& rwallet_state,
                                                  td::int32 revision, ton::WorkchainId workchain_id) {
  TRY_RESULT(init_data, to_init_data(rwallet_state));
  return ton::RestrictedWallet::create(init_data, revision)->get_address(workchain_id);
}

td::Result<td::Bits256> get_adnl_address(td::Slice adnl_address) {
  TRY_RESULT_PREFIX(address, td::adnl_id_decode(adnl_address),
                    TonlibError::InvalidField("adnl_address", "can't decode"));
  return address;
}

static td::optional<ton::SmartContractCode::Type> get_wallet_type(tonlib_api::InitialAccountState& state) {
  return downcast_call2<td::optional<ton::SmartContractCode::Type>>(
      state,
      td::overloaded(
          [](const tonlib_api::raw_initialAccountState&) { return td::optional<ton::SmartContractCode::Type>(); },
          [](const tonlib_api::wallet_v3_initialAccountState&) { return ton::SmartContractCode::WalletV3; },
          [](const tonlib_api::wallet_v4_initialAccountState&) { return ton::SmartContractCode::WalletV4; },
          [](const tonlib_api::wallet_highload_v1_initialAccountState&) {
            return ton::SmartContractCode::HighloadWalletV1;
          },
          [](const tonlib_api::wallet_highload_v2_initialAccountState&) {
            return ton::SmartContractCode::HighloadWalletV2;
          },
          [](const tonlib_api::rwallet_initialAccountState&) { return ton::SmartContractCode::RestrictedWallet; },
          [](const tonlib_api::pchan_initialAccountState&) { return ton::SmartContractCode::PaymentChannel; },
          [](const tonlib_api::dns_initialAccountState&) { return ton::SmartContractCode::ManualDns; }));
}

tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(
    const tonlib_api::getAccountAddress& request) {
  if (!request.initial_account_state_) {
    return status_to_tonlib_api(TonlibError::EmptyField("initial_account_state"));
  }
  auto o_type = get_wallet_type(*request.initial_account_state_);
  if (o_type) {
    auto status = ton::SmartContractCode::validate_revision(o_type.value(), request.revision_);
    if (status.is_error()) {
      return status_to_tonlib_api(TonlibError::InvalidRevision());
    }
  }
  auto r_account_address = downcast_call2<td::Result<block::StdAddress>>(
      *request.initial_account_state_,
      [&request](auto&& state) { return get_account_address(state, request.revision_, request.workchain_id_); });
  if (r_account_address.is_error()) {
    return status_to_tonlib_api(r_account_address.error());
  }
  return tonlib_api::make_object<tonlib_api::accountAddress>(r_account_address.ok().rserialize(true));
}

td::Status TonlibClient::do_request(tonlib_api::guessAccountRevision& request,
                                    td::Promise<object_ptr<tonlib_api::accountRevisionList>>&& promise) {
  std::vector<Target> targets;
  std::vector<tonlib_api::object_ptr<tonlib_api::InitialAccountState>> states;
  states.push_back(std::move(request.initial_account_state_));
  for (auto& initial_account_state : states) {
    if (!initial_account_state) {
      return TonlibError::EmptyField("initial_account_state");
    }
    auto o_type = get_wallet_type(*initial_account_state);
    if (!o_type) {
      continue;
    }
    auto type = o_type.unwrap();
    auto revisions = ton::SmartContractCode::get_revisions(type);
    auto workchains = std::vector<ton::WorkchainId>{request.workchain_id_};

    TRY_STATUS(downcast_call2<td::Status>(
        *initial_account_state, [&revisions, &targets, &workchains, &type](const auto& state) {
          for (auto workchain : workchains) {
            for (auto revision : revisions) {
              TRY_RESULT(address, get_account_address(state, revision, workchain));
              Target target;
              target.can_be_empty = type != ton::SmartContractCode::Type::RestrictedWallet;
              target.address = address;
              targets.push_back(std::move(target));
            }
          }
          return td::Status::OK();
        }));
  }

  return guess_revisions(std::move(targets), std::move(promise));
}

td::Status TonlibClient::do_request(tonlib_api::guessAccount& request,
                                    td::Promise<object_ptr<tonlib_api::accountRevisionList>>&& promise) {
  std::vector<Target> targets;
  struct Source {
    tonlib_api::object_ptr<tonlib_api::InitialAccountState> init_state;
    ton::WorkchainId workchain_id;
  };
  std::vector<Source> sources;
  std::string rwallet_init_public_key = request.rwallet_init_public_key_;
  if (rwallet_init_public_key.empty()) {
    rwallet_init_public_key = rwallet_init_public_key_;
  }
  TRY_RESULT(key_bytes, get_public_key(request.public_key_));
  sources.push_back(Source{tonlib_api::make_object<tonlib_api::rwallet_initialAccountState>(
                               rwallet_init_public_key, request.public_key_, wallet_id_ + ton::masterchainId),
                           ton::masterchainId});
  sources.push_back(Source{tonlib_api::make_object<tonlib_api::wallet_v3_initialAccountState>(
                               request.public_key_, wallet_id_ + ton::masterchainId),
                           ton::masterchainId});
  sources.push_back(Source{tonlib_api::make_object<tonlib_api::wallet_v3_initialAccountState>(
                               request.public_key_, wallet_id_ + ton::basechainId),
                           ton::basechainId});
  sources.push_back(Source{tonlib_api::make_object<tonlib_api::wallet_v4_initialAccountState>(
      request.public_key_, wallet_id_ + ton::masterchainId),
                           ton::masterchainId});
  sources.push_back(Source{tonlib_api::make_object<tonlib_api::wallet_v4_initialAccountState>(
      request.public_key_, wallet_id_ + ton::basechainId),
                           ton::basechainId});
  for (Source& source : sources) {
    auto o_type = get_wallet_type(*source.init_state);
    if (!o_type) {
      continue;
    }
    auto type = o_type.unwrap();
    auto revisions = ton::SmartContractCode::get_revisions(type);
    auto workchains = std::vector<ton::WorkchainId>{source.workchain_id};

    TRY_STATUS(downcast_call2<td::Status>(
        *source.init_state, [&revisions, &targets, &workchains, &type, &key_bytes](const auto& state) {
          for (auto workchain : workchains) {
            for (auto revision : revisions) {
              TRY_RESULT(address, get_account_address(state, revision, workchain));
              Target target;
              target.can_be_uninited =
                  type == ton::SmartContractCode::Type::WalletV3 && revision == 2 && workchain == ton::basechainId;
              target.can_be_empty = type != ton::SmartContractCode::Type::RestrictedWallet || target.can_be_uninited;
              target.address = address;
              target.public_key = td::Ed25519::PublicKey(td::SecureString(key_bytes.key));
              targets.push_back(std::move(target));
            }
          }
          return td::Status::OK();
        }));
  }

  return guess_revisions(std::move(targets), std::move(promise));
}

td::Status TonlibClient::guess_revisions(std::vector<Target> targets,
                                         td::Promise<object_ptr<tonlib_api::accountRevisionList>>&& promise) {
  auto actor_id = actor_id_++;
  class GuessRevisions : public TonlibQueryActor {
   public:
    GuessRevisions(td::actor::ActorShared<TonlibClient> client, td::optional<ton::BlockIdExt> block_id,
                   std::vector<Target> targets, td::Promise<std::vector<td::unique_ptr<AccountState>>> promise)
        : TonlibQueryActor(std::move(client))
        , block_id_(std::move(block_id))
        , targets_(std::move(targets))
        , promise_(std::move(promise)) {
    }

   private:
    td::optional<ton::BlockIdExt> block_id_;
    std::vector<Target> targets_;
    td::Promise<std::vector<td::unique_ptr<AccountState>>> promise_;

    size_t left_{1};
    std::vector<td::unique_ptr<AccountState>> res;

    void start_up() override {
      left_ += targets_.size();
      for (auto& p : targets_) {
        send_query(int_api::GetAccountState{p.address, block_id_.copy(), std::move(p.public_key)},
                   promise_send_closure(td::actor::actor_id(this), &GuessRevisions::on_account_state, std::move(p)));
      }
      on_account_state_finish();
    }
    void hangup() override {
      promise_.set_error(TonlibError::Cancelled());
      return;
    }
    void on_account_state(Target target, td::Result<td::unique_ptr<AccountState>> r_state) {
      if (!r_state.is_ok()) {
        promise_.set_error(r_state.move_as_error());
        stop();
        return;
      }
      SCOPE_EXIT {
        on_account_state_finish();
      };
      auto state = r_state.move_as_ok();
      if (state->get_balance() < 0 && !target.can_be_uninited) {
        return;
      }
      if (state->get_wallet_type() == AccountState::WalletType::Empty && !target.can_be_empty) {
        return;
      }

      res.push_back(std::move(state));
    }
    void on_account_state_finish() {
      left_--;
      if (left_ == 0) {
        std::sort(res.begin(), res.end(), [](auto& x, auto& y) {
          auto key = [](const td::unique_ptr<AccountState>& state) {
            return std::make_tuple(state->get_wallet_type() != AccountState::WalletType::Empty,
                                   state->get_wallet_type(), state->get_balance(), state->get_wallet_revision());
          };
          return key(x) > key(y);
        });
        promise_.set_value(std::move(res));
        stop();
      }
    }
  };

  actors_[actor_id] = td::actor::create_actor<GuessRevisions>(
      "GuessRevisions", actor_shared(this, actor_id), query_context_.block_id.copy(), std::move(targets),
      promise.wrap([](auto&& v) mutable {
        std::vector<tonlib_api::object_ptr<tonlib_api::fullAccountState>> res;
        for (auto& x : v) {
          auto r_state = x->to_fullAccountState();
          if (r_state.is_error()) {
            LOG(ERROR) << "to_fullAccountState failed: " << r_state.error();
            continue;
          }
          res.push_back(r_state.move_as_ok());
        }
        return tonlib_api::make_object<tonlib_api::accountRevisionList>(std::move(res));
      }));
  return td::Status::OK();
}

tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(
    const tonlib_api::unpackAccountAddress& request) {
  auto r_account_address = get_account_address(request.account_address_);
  if (r_account_address.is_error()) {
    return status_to_tonlib_api(r_account_address.move_as_error());
  }
  auto account_address = r_account_address.move_as_ok();
  return tonlib_api::make_object<tonlib_api::unpackedAccountAddress>(
      account_address.workchain, account_address.bounceable, account_address.testnet,
      account_address.addr.as_slice().str());
}

tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(
    const tonlib_api::packAccountAddress& request) {
  if (!request.account_address_) {
    return status_to_tonlib_api(TonlibError::EmptyField("account_address"));
  }
  if (request.account_address_->addr_.size() != 32) {
    return status_to_tonlib_api(TonlibError::InvalidField("account_address.addr", "must be 32 bytes long"));
  }
  block::StdAddress addr;
  addr.workchain = request.account_address_->workchain_id_;
  addr.bounceable = request.account_address_->bounceable_;
  addr.testnet = request.account_address_->testnet_;
  addr.addr.as_slice().copy_from(request.account_address_->addr_);
  return tonlib_api::make_object<tonlib_api::accountAddress>(addr.rserialize(true));
}

tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(tonlib_api::getBip39Hints& request) {
  return tonlib_api::make_object<tonlib_api::bip39Hints>(
      td::transform(Mnemonic::word_hints(td::trim(td::to_lower_inplace(request.prefix_))), [](auto& x) { return x; }));
}

td::Status TonlibClient::do_request(const tonlib_api::init& request,
                                    td::Promise<object_ptr<tonlib_api::options_info>>&& promise) {
  if (state_ != State::Uninited) {
    return td::Status::Error(400, "Tonlib is already inited");
  }
  if (!request.options_) {
    return TonlibError::EmptyField("options");
  }
  if (!request.options_->keystore_type_) {
    return TonlibError::EmptyField("options.keystore_type");
  }

  auto r_kv = downcast_call2<td::Result<td::unique_ptr<KeyValue>>>(
      *request.options_->keystore_type_,
      td::overloaded(
          [](tonlib_api::keyStoreTypeDirectory& directory) { return KeyValue::create_dir(directory.directory_); },
          [](tonlib_api::keyStoreTypeInMemory& inmemory) { return KeyValue::create_inmemory(); }));
  TRY_RESULT(kv, std::move(r_kv));
  kv_ = std::shared_ptr<KeyValue>(kv.release());

  load_libs_from_disk();

  key_storage_.set_key_value(kv_);
  last_block_storage_.set_key_value(kv_);
  auto res = tonlib_api::make_object<tonlib_api::options_info>();
  if (request.options_->config_) {
    TRY_RESULT(full_config, validate_config(std::move(request.options_->config_)));
    res->config_info_ = to_tonlib_api(full_config);
    set_config(std::move(full_config));
  }
  state_ = State::Running;
  promise.set_value(std::move(res));
  return td::Status::OK();
}

class MasterConfig {
 public:
  void add_config(std::string name, std::string json) {
    auto config = std::make_shared<Config>(Config::parse(json).move_as_ok());
    config->name = name;
    if (!name.empty()) {
      by_name_[name] = config;
    }
    by_root_hash_[config->zero_state_id.root_hash] = config;
  }
  td::optional<Config> by_name(std::string name) const {
    auto it = by_name_.find(name);
    if (it == by_name_.end()) {
      return {};
    }
    return *it->second;
  }

  td::optional<Config> by_root_hash(const ton::RootHash& root_hash) const {
    auto it = by_root_hash_.find(root_hash);
    if (it == by_root_hash_.end()) {
      return {};
    }
    return *it->second;
  }

 private:
  size_t next_id_{0};
  std::map<std::string, std::shared_ptr<const Config>> by_name_;
  std::map<ton::RootHash, std::shared_ptr<const Config>> by_root_hash_;
};

const MasterConfig& get_default_master_config() {
  static MasterConfig config = [] {
    MasterConfig res;
    res.add_config("mainnet", R"abc({
      "liteservers": [
      ],
      "validator": {
        "@type": "validator.config.global",
        "zero_state": {
          "workchain": -1,
          "shard": -9223372036854775808,
          "seqno": 0,
          "root_hash": "F6OpKZKqvqeFp6CQmFomXNMfMj2EnaUSOXN+Mh+wVWk=",
          "file_hash": "XplPz01CXAps5qeSWUtxcyBfdAo5zVb1N979KLSKD24="
        },
        "init_block" : {
          "root_hash": "YRkrcmZMvLBvjanwKCyL3w4oceGPtFfgx8ym1QKCK/4=",
          "seqno": 27747086,
          "file_hash": "N42xzPnJjDlE3hxPXOb+pNzXomgRtpX5AZzMPnIA41s=",
          "workchain": -1,
          "shard": -9223372036854775808
        },
        "hardforks": [
          {
            "file_hash": "t/9VBPODF7Zdh4nsnA49dprO69nQNMqYL+zk5bCjV/8=",
            "seqno": 8536841,
            "root_hash": "08Kpc9XxrMKC6BF/FeNHPS3MEL1/Vi/fQU/C9ELUrkc=",
            "workchain": -1,
            "shard": -9223372036854775808
          }
        ]
      }
    })abc");
    res.add_config("testnet", R"abc({
      "liteservers": [
      ],
      "validator": {
      		"zero_state": {
      			"file_hash": "Z+IKwYS54DmmJmesw/nAD5DzWadnOCMzee+kdgSYDOg=",
      			"seqno": 0,
      			"root_hash": "gj+B8wb/AmlPk1z1AhVI484rhrUpgSr2oSFIh56VoSg=",
      			"workchain": -1,
      			"shard": -9223372036854775808
      		},
      		"@type": "validator.config.global",
      		"init_block":
      		      {
      			"file_hash": "xRaxgUwgTXYFb16YnR+Q+VVsczLl6jmYwvzhQ/ncrh4=",
      			"seqno": 5176527,
      			"root_hash": "SoPLqMe9Dz26YJPOGDOHApTSe5i0kXFtRmRh/zPMGuI=",
      			"workchain": -1,
      			"shard": -9223372036854775808
      		      },
      		"hardforks": [
      		      {
      			"file_hash": "jF3RTD+OyOoP+OI9oIjdV6M8EaOh9E+8+c3m5JkPYdg=",
      			"seqno": 5141579,
      			"root_hash": "6JSqIYIkW7y8IorxfbQBoXiuY3kXjcoYgQOxTJpjXXA=",
      			"workchain": -1,
      			"shard": -9223372036854775808
      		      },
      		      {
      			"file_hash": "WrNoMrn5UIVPDV/ug/VPjYatvde8TPvz5v1VYHCLPh8=",
      			"seqno": 5172980,
      			"root_hash": "054VCNNtUEwYGoRe1zjH+9b1q21/MeM+3fOo76Vcjes=",
      			"workchain": -1,
      			"shard": -9223372036854775808
      		      },
      		      {
      			"file_hash": "xRaxgUwgTXYFb16YnR+Q+VVsczLl6jmYwvzhQ/ncrh4=",
      			"seqno": 5176527,
      			"root_hash": "SoPLqMe9Dz26YJPOGDOHApTSe5i0kXFtRmRh/zPMGuI=",
      			"workchain": -1,
      			"shard": -9223372036854775808
      		      }
      		    ]
      	}
    })abc");
    return res;
  }();
  return config;
}

td::Result<TonlibClient::FullConfig> TonlibClient::validate_config(tonlib_api::object_ptr<tonlib_api::config> config) {
  if (!config) {
    return TonlibError::EmptyField("config");
  }
  if (config->config_.empty()) {
    return TonlibError::InvalidConfig("config is empty");
  }
  TRY_RESULT_PREFIX(new_config, Config::parse(std::move(config->config_)),
                    TonlibError::InvalidConfig("can't parse config"));

  if (new_config.lite_servers.empty() && !config->use_callbacks_for_network_) {
    return TonlibError::InvalidConfig("no lite clients");
  }
  td::optional<Config> o_master_config;
  std::string last_state_key;
  if (config->blockchain_name_.empty()) {
    last_state_key = new_config.zero_state_id.root_hash.as_slice().str();
    o_master_config = get_default_master_config().by_root_hash(new_config.zero_state_id.root_hash);
  } else {
    last_state_key = config->blockchain_name_;
    new_config.name = config->blockchain_name_;
    o_master_config = get_default_master_config().by_name(config->blockchain_name_);
    if (!o_master_config) {
      o_master_config = get_default_master_config().by_root_hash(new_config.zero_state_id.root_hash);
    }
  }

  if (o_master_config) {
    auto name = o_master_config.value().name;
    if (!name.empty() && !new_config.name.empty() && new_config.name != name && name == "mainnet") {
      return TonlibError::InvalidConfig(PSLICE() << "Invalid blockchain_id: expected '" << name << "'");
    }
  }

  if (o_master_config && o_master_config.value().zero_state_id != new_config.zero_state_id) {
    return TonlibError::InvalidConfig("zero_state differs from embedded zero_state");
  }

  if (o_master_config && o_master_config.value().hardforks != new_config.hardforks) {
    return TonlibError::InvalidConfig("hardforks differs from embedded hardforks");
  }

  int vert_seqno = static_cast<int>(new_config.hardforks.size());

  LastBlockState state;
  td::Result<LastBlockState> r_state;
  if (!config->ignore_cache_) {
    r_state = last_block_storage_.get_state(last_state_key);
  }
  auto zero_state = ton::ZeroStateIdExt(new_config.zero_state_id.id.workchain, new_config.zero_state_id.root_hash,
                                        new_config.zero_state_id.file_hash);
  if (config->ignore_cache_ || r_state.is_error()) {
    LOG_IF(WARNING, !config->ignore_cache_) << "Unknown LastBlockState: " << r_state.error();
    state.zero_state_id = zero_state;
    state.last_block_id = new_config.zero_state_id;
    state.last_key_block_id = new_config.zero_state_id;
  } else {
    state = r_state.move_as_ok();
    if (state.zero_state_id != zero_state) {
      LOG(ERROR) << state.zero_state_id.to_str() << " " << zero_state.to_str();
      return TonlibError::InvalidConfig("zero_state differs from cached zero_state");
    }
    if (state.vert_seqno > vert_seqno) {
      LOG(ERROR) << "Stored vert_seqno is bigger than one in config: " << state.vert_seqno << " vs " << vert_seqno;
      return TonlibError::InvalidConfig("vert_seqno in cached state is bigger");
    }
    if (state.vert_seqno < vert_seqno) {
      state.zero_state_id = zero_state;
      state.last_block_id = new_config.zero_state_id;
      state.last_key_block_id = new_config.zero_state_id;
      state.init_block_id = ton::BlockIdExt{};
      LOG(WARNING) << "Drop cached state - vert_seqno is smaller than in config";
    }
  }
  state.vert_seqno = vert_seqno;

  bool user_defined_init_block = false;
  if (new_config.init_block_id.is_valid() &&
      state.last_key_block_id.id.seqno < new_config.init_block_id.id.seqno) {
    state.last_key_block_id = new_config.init_block_id;
    user_defined_init_block = true;
    LOG(INFO) << "Use init block from USER config: " << new_config.init_block_id.to_str();
  }

  if (o_master_config && !user_defined_init_block) {
    auto master_config = o_master_config.unwrap();
    if (master_config.init_block_id.is_valid() &&
        state.last_key_block_id.id.seqno < master_config.init_block_id.id.seqno) {
      state.last_key_block_id = master_config.init_block_id;
      LOG(INFO) << "Use init block from MASTER config: " << master_config.init_block_id.to_str();
    }
    if (!master_config.name.empty()) {
      if (new_config.name != master_config.name) {
        LOG(INFO) << "Use blockchain name from MASTER config: '" << master_config.name << "' (was '" << new_config.name
                  << "')";
        new_config.name = master_config.name;
      }
    }
  }

  FullConfig res;
  res.config = std::move(new_config);
  res.use_callbacks_for_network = config->use_callbacks_for_network_;
  res.wallet_id = td::as<td::uint32>(res.config.zero_state_id.root_hash.as_slice().data());
  res.rwallet_init_public_key = "Puasxr0QfFZZnYISRphVse7XHKfW7pZU5SJarVHXvQ+rpzkD";
  res.last_state_key = std::move(last_state_key);
  res.last_state = std::move(state);

  return std::move(res);
}

void TonlibClient::set_config(FullConfig full_config) {
  config_ = std::move(full_config.config);
  config_generation_++;
  wallet_id_ = full_config.wallet_id;
  rwallet_init_public_key_ = full_config.rwallet_init_public_key;
  last_state_key_ = full_config.last_state_key;

  use_callbacks_for_network_ = full_config.use_callbacks_for_network;
  init_ext_client();
  init_last_block(std::move(full_config.last_state));
  init_last_config();
  client_.set_client(get_client_ref());
}

td::Status TonlibClient::do_request(const tonlib_api::close& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  CHECK(state_ != State::Closed);
  state_ = State::Closed;
  source_.cancel();
  promise.set_value(tonlib_api::make_object<tonlib_api::ok>());
  return td::Status::OK();
}

td::Status TonlibClient::do_request(tonlib_api::options_validateConfig& request,
                                    td::Promise<object_ptr<tonlib_api::options_configInfo>>&& promise) {
  TRY_RESULT(config, validate_config(std::move(request.config_)));
  auto res = to_tonlib_api(config);
  promise.set_value(std::move(res));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(tonlib_api::options_setConfig& request,
                                    td::Promise<object_ptr<tonlib_api::options_configInfo>>&& promise) {
  if (!request.config_) {
    return TonlibError::EmptyField("config");
  }
  TRY_RESULT(config, validate_config(std::move(request.config_)));
  auto res = to_tonlib_api(config);
  set_config(std::move(config));
  promise.set_value(std::move(res));
  return td::Status::OK();
}

td::Result<std::string> to_std_address_or_throw(td::Ref<vm::CellSlice> cs) {
  auto tag = block::gen::MsgAddressInt().get_tag(*cs);
  if (tag < 0) {
    return td::Status::Error("Failed to read MsgAddressInt tag");
  }
  if (tag != block::gen::MsgAddressInt::addr_std) {
    return "";
  }
  block::gen::MsgAddressInt::Record_addr_std addr;
  if (!tlb::csr_unpack(cs, addr)) {
    return td::Status::Error("Failed to unpack MsgAddressInt");
  }
  return block::StdAddress(addr.workchain_id, addr.address).rserialize(true);
}

td::Result<std::string> to_std_address(td::Ref<vm::CellSlice> cs) {
  return TRY_VM(to_std_address_or_throw(std::move(cs)));
}
struct ToRawTransactions {
  explicit ToRawTransactions(td::optional<td::Ed25519::PrivateKey> private_key, bool try_decode_messages = true)
    : private_key_(std::move(private_key))
    , try_decode_messages_(try_decode_messages) {
  }

  td::optional<td::Ed25519::PrivateKey> private_key_;
  bool try_decode_messages_;

  td::Result<tonlib_api::object_ptr<tonlib_api::raw_message>> to_raw_message_or_throw(td::Ref<vm::Cell> cell) {
    block::gen::Message::Record message;
    if (!tlb::type_unpack_cell(cell, block::gen::t_Message_Any, message)) {
      return td::Status::Error("Failed to unpack Message");
    }

    td::Ref<vm::CellSlice> body;
    if (message.body->prefetch_long(1) == 0) {
      body = std::move(message.body);
      body.write().advance(1);
    } else {
      body = vm::load_cell_slice_ref(message.body->prefetch_ref());
    }
    auto body_cell = vm::CellBuilder().append_cellslice(*body).finalize();
    auto body_hash = body_cell->get_hash().as_slice().str();
    auto msg_hash = cell->get_hash().as_slice().str();

    td::Ref<vm::Cell> init_state_cell;
    auto& init_state_cs = message.init.write();
    if (init_state_cs.fetch_ulong(1) == 1) {
      if (init_state_cs.fetch_long(1) == 0) {
        init_state_cell = vm::CellBuilder().append_cellslice(init_state_cs).finalize();
      } else {
        init_state_cell = init_state_cs.fetch_ref();
      }
    }

    auto get_data = [body = std::move(body), body_cell = std::move(body_cell),
                     init_state_cell = std::move(init_state_cell), this](td::Slice salt) mutable {
      tonlib_api::object_ptr<tonlib_api::msg_Data> data;
      if (try_decode_messages_ && body->size() >= 32) {
        auto type = static_cast<td::uint32>(body.write().fetch_long(32));
        if (type == 0 || type == ton::WalletInterface::EncryptedCommentOp) {
          td::Status status;

          auto r_body_message = TRY_VM(vm::CellString::load(body.write()));
          LOG_IF(WARNING, r_body_message.is_error()) << "Failed to parse a message: " << r_body_message.error();

          if (r_body_message.is_ok()) {
            if (type == 0) {
              data = tonlib_api::make_object<tonlib_api::msg_dataText>(r_body_message.move_as_ok());
            } else {
              auto encrypted_message = r_body_message.move_as_ok();
              auto r_decrypted_message = [&]() -> td::Result<std::string> {
                if (!private_key_) {
                  return TonlibError::EmptyField("private_key");
                }
                TRY_RESULT(decrypted, SimpleEncryptionV2::decrypt_data(encrypted_message, private_key_.value(), salt));
                return decrypted.data.as_slice().str();
              }();
              if (r_decrypted_message.is_ok()) {
                data = tonlib_api::make_object<tonlib_api::msg_dataDecryptedText>(r_decrypted_message.move_as_ok());
              } else {
                data = tonlib_api::make_object<tonlib_api::msg_dataEncryptedText>(encrypted_message);
              }
            }
          }
        }
      }
      if (!data) {
        data = tonlib_api::make_object<tonlib_api::msg_dataRaw>(to_bytes(std::move(body_cell)), to_bytes(std::move(init_state_cell)));
      }
      return data;
    };

    auto tag = block::gen::CommonMsgInfo().get_tag(*message.info);
    if (tag < 0) {
      return td::Status::Error("Failed to read CommonMsgInfo tag");
    }
    switch (tag) {
      case block::gen::CommonMsgInfo::int_msg_info: {
        block::gen::CommonMsgInfo::Record_int_msg_info msg_info;
        if (!tlb::csr_unpack(message.info, msg_info)) {
          return td::Status::Error("Failed to unpack CommonMsgInfo::int_msg_info");
        }

        TRY_RESULT(balance, to_balance(msg_info.value));
        TRY_RESULT(extra_currencies, parse_extra_currencies(msg_info.value->prefetch_ref()));
        TRY_RESULT(src, to_std_address(msg_info.src));
        TRY_RESULT(dest, to_std_address(msg_info.dest));
        TRY_RESULT(fwd_fee, to_balance(msg_info.fwd_fee));
        TRY_RESULT(ihr_fee, to_balance(msg_info.ihr_fee));
        auto created_lt = static_cast<td::int64>(msg_info.created_lt);

        return tonlib_api::make_object<tonlib_api::raw_message>(
            msg_hash,
            tonlib_api::make_object<tonlib_api::accountAddress>(src),
            tonlib_api::make_object<tonlib_api::accountAddress>(std::move(dest)), balance,
            std::move(extra_currencies), fwd_fee, ihr_fee, created_lt, std::move(body_hash),
            get_data(src));
      }
      case block::gen::CommonMsgInfo::ext_in_msg_info: {
        block::gen::CommonMsgInfo::Record_ext_in_msg_info msg_info;
        if (!tlb::csr_unpack(message.info, msg_info)) {
          return td::Status::Error("Failed to unpack CommonMsgInfo::ext_in_msg_info");
        }
        TRY_RESULT(dest, to_std_address(msg_info.dest));
        return tonlib_api::make_object<tonlib_api::raw_message>(
            msg_hash,
            tonlib_api::make_object<tonlib_api::accountAddress>(),
            tonlib_api::make_object<tonlib_api::accountAddress>(std::move(dest)), 0,
            std::vector<tonlib_api::object_ptr<tonlib_api::extraCurrency>>{}, 0, 0, 0, std::move(body_hash),
            get_data(""));
      }
      case block::gen::CommonMsgInfo::ext_out_msg_info: {
        block::gen::CommonMsgInfo::Record_ext_out_msg_info msg_info;
        if (!tlb::csr_unpack(message.info, msg_info)) {
          return td::Status::Error("Failed to unpack CommonMsgInfo::ext_out_msg_info");
        }
        TRY_RESULT(src, to_std_address(msg_info.src));
        auto created_lt = static_cast<td::int64>(msg_info.created_lt);
        return tonlib_api::make_object<tonlib_api::raw_message>(
            msg_hash,
            tonlib_api::make_object<tonlib_api::accountAddress>(src),
            tonlib_api::make_object<tonlib_api::accountAddress>(), 0,
            std::vector<tonlib_api::object_ptr<tonlib_api::extraCurrency>>{}, 0, 0, created_lt, std::move(body_hash),
            get_data(src));
      }
    }

    return td::Status::Error("Unknown CommonMsgInfo tag");
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::raw_message>> to_raw_message(td::Ref<vm::Cell> cell) {
    return TRY_VM(to_raw_message_or_throw(std::move(cell)));
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::raw_transaction>> to_raw_transaction_or_throw(
      block::Transaction::Info&& info) {
    std::string data;

    tonlib_api::object_ptr<tonlib_api::raw_message> in_msg;
    std::vector<tonlib_api::object_ptr<tonlib_api::raw_message>> out_msgs;
    td::int64 fees = 0;
    td::int64 storage_fee = 0;
    td::string address;
    if (info.transaction.not_null()) {
      data = to_bytes(info.transaction);
      block::gen::Transaction::Record trans;
      if (!tlb::unpack_cell(info.transaction, trans)) {
        return td::Status::Error("Failed to unpack Transaction");
      }

      TRY_RESULT_ASSIGN(fees, to_balance(trans.total_fees));

      auto is_just = trans.r1.in_msg->prefetch_long(1);
      if (is_just == trans.r1.in_msg->fetch_long_eof) {
        return td::Status::Error("Failed to parse long");
      }
      if (is_just == -1) {
        auto msg = trans.r1.in_msg->prefetch_ref();
        TRY_RESULT(in_msg_copy, to_raw_message(trans.r1.in_msg->prefetch_ref()));
        in_msg = std::move(in_msg_copy);
      }

      if (trans.outmsg_cnt != 0) {
        vm::Dictionary dict{trans.r1.out_msgs, 15};
        for (int x = 0; x < trans.outmsg_cnt; x++) {
          TRY_RESULT(out_msg, to_raw_message(dict.lookup_ref(td::BitArray<15>{x})));
          fees += out_msg->fwd_fee_;
          fees += out_msg->ihr_fee_;
          out_msgs.push_back(std::move(out_msg));
        }
      }
      td::RefInt256 storage_fees;
      if (!block::tlb::t_TransactionDescr.get_storage_fees(trans.description, storage_fees)) {
        return td::Status::Error("Failed to fetch storage fee from transaction");
      }
      storage_fee = storage_fees->to_long();
      auto std_address = block::StdAddress(info.blkid.id.workchain, trans.account_addr);
      address = std_address.rserialize(true);
    }
    return tonlib_api::make_object<tonlib_api::raw_transaction>(
        tonlib_api::make_object<tonlib_api::accountAddress>(std::move(address)),
        info.now, data,
        tonlib_api::make_object<tonlib_api::internal_transactionId>(info.prev_trans_lt,
                                                                    info.prev_trans_hash.as_slice().str()),
        fees, storage_fee, fees - storage_fee, std::move(in_msg), std::move(out_msgs));
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::raw_transaction>> to_raw_transaction(block::Transaction::Info&& info) {
    return TRY_VM(to_raw_transaction_or_throw(std::move(info)));
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::raw_transactions>> to_raw_transactions(
      block::TransactionList::Info&& info) {
    std::vector<tonlib_api::object_ptr<tonlib_api::raw_transaction>> transactions;
    for (auto& transaction : info.transactions) {
      TRY_RESULT(raw_transaction, to_raw_transaction(std::move(transaction)));
      transactions.push_back(std::move(raw_transaction));
    }

    auto transaction_id =
        tonlib_api::make_object<tonlib_api::internal_transactionId>(info.lt, info.hash.as_slice().str());
    for (auto& transaction : transactions) {
      std::swap(transaction->transaction_id_, transaction_id);
    }

    return tonlib_api::make_object<tonlib_api::raw_transactions>(std::move(transactions), std::move(transaction_id));
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::raw_transaction>> to_raw_transaction_or_throw(
      block::BlockTransaction::Info&& info) {
    std::string data;

    tonlib_api::object_ptr<tonlib_api::raw_message> in_msg;
    std::vector<tonlib_api::object_ptr<tonlib_api::raw_message>> out_msgs;
    td::int64 fees = 0;
    td::int64 storage_fee = 0;
    td::string address;
    if (info.transaction.not_null()) {
      data = to_bytes(info.transaction);
      block::gen::Transaction::Record trans;
      if (!tlb::unpack_cell(info.transaction, trans)) {
        return td::Status::Error("Failed to unpack Transaction");
      }

      TRY_RESULT_ASSIGN(fees, to_balance(trans.total_fees));

      auto is_just = trans.r1.in_msg->prefetch_long(1);
      if (is_just == trans.r1.in_msg->fetch_long_eof) {
        return td::Status::Error("Failed to parse long");
      }
      if (is_just == -1) {
        auto msg = trans.r1.in_msg->prefetch_ref();
        TRY_RESULT(in_msg_copy, to_raw_message(trans.r1.in_msg->prefetch_ref()));
        in_msg = std::move(in_msg_copy);
      }

      if (trans.outmsg_cnt != 0) {
        vm::Dictionary dict{trans.r1.out_msgs, 15};
        for (int x = 0; x < trans.outmsg_cnt; x++) {
          TRY_RESULT(out_msg, to_raw_message(dict.lookup_ref(td::BitArray<15>{x})));
          fees += out_msg->fwd_fee_;
          fees += out_msg->ihr_fee_;
          out_msgs.push_back(std::move(out_msg));
        }
      }
      td::RefInt256 storage_fees;
      if (!block::tlb::t_TransactionDescr.get_storage_fees(trans.description, storage_fees)) {
        return td::Status::Error("Failed to fetch storage fee from transaction");
      }
      storage_fee = storage_fees->to_long();
      auto std_address = block::StdAddress(info.blkid.id.workchain, trans.account_addr);
      address = std_address.rserialize(true);
    }
    return tonlib_api::make_object<tonlib_api::raw_transaction>(
        tonlib_api::make_object<tonlib_api::accountAddress>(std::move(address)),
        info.now, data,
        tonlib_api::make_object<tonlib_api::internal_transactionId>(info.lt,
                                                                    info.hash.as_slice().str()),
        fees, storage_fee, fees - storage_fee, std::move(in_msg), std::move(out_msgs));
  }

  td::Result<tonlib_api::object_ptr<tonlib_api::raw_transaction>> to_raw_transaction(block::BlockTransaction::Info&& info) {
    return TRY_VM(to_raw_transaction_or_throw(std::move(info)));
  }

  td::Result<std::vector<tonlib_api::object_ptr<tonlib_api::raw_transaction>>> to_raw_transactions(
      block::BlockTransactionList::Info&& info) {
    std::vector<tonlib_api::object_ptr<tonlib_api::raw_transaction>> transactions;
    for (auto& transaction : info.transactions) {
      TRY_RESULT(raw_transaction, to_raw_transaction(std::move(transaction)));
      transactions.push_back(std::move(raw_transaction));
    }

    return std::move(transactions);
  }
};

// Raw

auto to_any_promise(td::Promise<tonlib_api::object_ptr<tonlib_api::ok>>&& promise) {
  return promise.wrap([](auto x) { return tonlib_api::make_object<tonlib_api::ok>(); });
}
auto to_any_promise(td::Promise<td::Unit>&& promise) {
  return promise.wrap([](auto x) { return td::Unit(); });
}

td::Status TonlibClient::do_request(const tonlib_api::raw_sendMessage& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  TRY_RESULT_PREFIX(body, vm::std_boc_deserialize(request.body_), TonlibError::InvalidBagOfCells("body"));
  std::ostringstream os;
  block::gen::t_Message_Any.print_ref(os, body);
  LOG(ERROR) << os.str();
  make_request(int_api::SendMessage{std::move(body)}, to_any_promise(std::move(promise)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::raw_sendMessageReturnHash& request,
                                    td::Promise<object_ptr<tonlib_api::raw_extMessageInfo>>&& promise) {
  TRY_RESULT_PREFIX(body, vm::std_boc_deserialize(request.body_), TonlibError::InvalidBagOfCells("body"));
  auto hash = body->get_hash().as_slice().str();
  make_request(int_api::SendMessage{std::move(body)},
    promise.wrap([hash = std::move(hash)](auto res) {
      return tonlib_api::make_object<tonlib_api::raw_extMessageInfo>(std::move(hash));
    }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::raw_createAndSendMessage& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  td::Ref<vm::Cell> init_state;
  if (!request.initial_account_state_.empty()) {
    TRY_RESULT_PREFIX(new_init_state, vm::std_boc_deserialize(request.initial_account_state_),
                      TonlibError::InvalidBagOfCells("initial_account_state"));
    init_state = std::move(new_init_state);
  }
  TRY_RESULT_PREFIX(data, vm::std_boc_deserialize(request.data_), TonlibError::InvalidBagOfCells("data"));
  TRY_RESULT(account_address, get_account_address(request.destination_->account_address_));
  auto message = ton::GenericAccount::create_ext_message(account_address, std::move(init_state), std::move(data));

  make_request(int_api::SendMessage{std::move(message)}, to_any_promise(std::move(promise)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(tonlib_api::raw_getAccountState& request,
                                    td::Promise<object_ptr<tonlib_api::raw_fullAccountState>>&& promise) {
  if (!request.account_address_) {
    return TonlibError::EmptyField("account_address");
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  make_request(int_api::GetAccountState{std::move(account_address), query_context_.block_id.copy(), {}},
               promise.wrap([](auto&& res) { return res->to_raw_fullAccountState(); }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(tonlib_api::raw_getAccountStateByTransaction& request,
                                    td::Promise<object_ptr<tonlib_api::raw_fullAccountState>>&& promise) {
  if (!request.account_address_) {
    return TonlibError::EmptyField("account_address");
  }
  if (!request.transaction_id_) {
    return TonlibError::EmptyField("transaction_id");
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  auto lt = request.transaction_id_->lt_;
  auto hash_str = request.transaction_id_->hash_;
  if (hash_str.size() != 32) {
    return td::Status::Error(400, "Invalid transaction id hash size");
  }
  td::Bits256 hash;
  hash.as_slice().copy_from(hash_str);
  make_request(int_api::GetAccountStateByTransaction{account_address, lt, hash},
               promise.wrap([](auto&& res) { return res->to_raw_fullAccountState(); }));
  return td::Status::OK();
}

td::Result<KeyStorage::InputKey> from_tonlib(tonlib_api::inputKeyRegular& input_key) {
  if (!input_key.key_) {
    return TonlibError::EmptyField("key");
  }

  TRY_RESULT(key_bytes, get_public_key(input_key.key_->public_key_));
  return KeyStorage::InputKey{{td::SecureString(key_bytes.key), std::move(input_key.key_->secret_)},
                              std::move(input_key.local_password_)};
}

td::Result<KeyStorage::InputKey> from_tonlib(tonlib_api::InputKey& input_key) {
  return downcast_call2<td::Result<KeyStorage::InputKey>>(
      input_key, td::overloaded([&](tonlib_api::inputKeyRegular& input_key) { return from_tonlib(input_key); },
                                [&](tonlib_api::inputKeyFake&) { return KeyStorage::fake_input_key(); }));
}

td::Status TonlibClient::do_request(tonlib_api::raw_getTransactions& request,
                                    td::Promise<object_ptr<tonlib_api::raw_transactions>>&& promise) {
  if (!request.account_address_) {
    return TonlibError::EmptyField("account_address");
  }
  if (!request.from_transaction_id_) {
    return TonlibError::EmptyField("from_transaction_id");
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  td::optional<td::Ed25519::PrivateKey> private_key;
  if (request.private_key_) {
    TRY_RESULT(input_key, from_tonlib(*request.private_key_));
    //NB: optional<Status> has lot of problems. We use emplace to migitate them
    td::optional<td::Status> o_status;
    //NB: rely on (and assert) that GetPrivateKey is a synchonous request
    make_request(int_api::GetPrivateKey{std::move(input_key)}, [&](auto&& r_key) {
      if (r_key.is_error()) {
        o_status.emplace(r_key.move_as_error());
        return;
      }
      o_status.emplace(td::Status::OK());
      private_key = td::Ed25519::PrivateKey(std::move(r_key.move_as_ok().private_key));
    });
    TRY_STATUS(o_status.unwrap());
  }
  auto lt = request.from_transaction_id_->lt_;
  auto hash_str = request.from_transaction_id_->hash_;
  if (hash_str.size() != 32) {
    return td::Status::Error(400, "Invalid transaction id hash size");
  }
  td::Bits256 hash;
  hash.as_slice().copy_from(hash_str);

  auto actor_id = actor_id_++;
  actors_[actor_id] = td::actor::create_actor<GetTransactionHistory>(
      "GetTransactionHistory", client_.get_client(), account_address, lt, hash, 10, actor_shared(this, actor_id),
      promise.wrap([private_key = std::move(private_key)](auto&& x) mutable {
        return ToRawTransactions(std::move(private_key)).to_raw_transactions(std::move(x));
      }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(tonlib_api::raw_getTransactionsV2& request,
                        td::Promise<object_ptr<tonlib_api::raw_transactions>>&& promise) {
  if (!request.account_address_) {
    return TonlibError::EmptyField("account_address");
  }
  if (!request.from_transaction_id_) {
    return TonlibError::EmptyField("from_transaction_id");
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  td::optional<td::Ed25519::PrivateKey> private_key;
  if (request.private_key_) {
    TRY_RESULT(input_key, from_tonlib(*request.private_key_));
    //NB: optional<Status> has lot of problems. We use emplace to migitate them
    td::optional<td::Status> o_status;
    //NB: rely on (and assert) that GetPrivateKey is a synchonous request
    make_request(int_api::GetPrivateKey{std::move(input_key)}, [&](auto&& r_key) {
      if (r_key.is_error()) {
        o_status.emplace(r_key.move_as_error());
        return;
      }
      o_status.emplace(td::Status::OK());
      private_key = td::Ed25519::PrivateKey(std::move(r_key.move_as_ok().private_key));
    });
    TRY_STATUS(o_status.unwrap());
  }
  auto lt = request.from_transaction_id_->lt_;
  auto hash_str = request.from_transaction_id_->hash_;
  if (hash_str.size() != 32) {
    return td::Status::Error(400, "Invalid transaction id hash size");
  }
  td::Bits256 hash;
  hash.as_slice().copy_from(hash_str);
  td::int32 count = request.count_ ? request.count_ : 10;

  auto actor_id = actor_id_++;
  actors_[actor_id] = td::actor::create_actor<GetTransactionHistory>(
      "GetTransactionHistory", client_.get_client(), account_address, lt, hash, count, actor_shared(this, actor_id),
      promise.wrap([private_key = std::move(private_key), try_decode_messages = request.try_decode_messages_](auto&& x) mutable {
        return ToRawTransactions(std::move(private_key), try_decode_messages).to_raw_transactions(std::move(x));
      }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::getAccountState& request,
                                    td::Promise<object_ptr<tonlib_api::fullAccountState>>&& promise) {
  if (!request.account_address_) {
    return TonlibError::EmptyField("account_address");
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  make_request(int_api::GetAccountState{std::move(account_address), query_context_.block_id.copy(), {}},
               promise.wrap([](auto&& res) { return res->to_fullAccountState(); }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::getAccountStateByTransaction& request,
                                    td::Promise<object_ptr<tonlib_api::fullAccountState>>&& promise) {
  if (!request.account_address_) {
    return TonlibError::EmptyField("account_address");
  }
  if (!request.transaction_id_) {
    return TonlibError::EmptyField("transaction_id");
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  auto lt = request.transaction_id_->lt_;
  auto hash_str = request.transaction_id_->hash_;
  if (hash_str.size() != 32) {
    return td::Status::Error(400, "Invalid transaction id hash size");
  }
  td::Bits256 hash;
  hash.as_slice().copy_from(hash_str);
  make_request(int_api::GetAccountStateByTransaction{account_address, lt, hash},
               promise.wrap([](auto&& res) { return res->to_fullAccountState(); }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::getShardAccountCell& request,
                                    td::Promise<object_ptr<tonlib_api::tvm_cell>>&& promise) {
  if (!request.account_address_) {
    return TonlibError::EmptyField("account_address");
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  make_request(int_api::GetAccountState{std::move(account_address), query_context_.block_id.copy(), {}},
               promise.wrap([](auto&& res) { return res->to_shardAccountCell(); }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::getShardAccountCellByTransaction& request,
                                    td::Promise<object_ptr<tonlib_api::tvm_cell>>&& promise) {
  if (!request.account_address_) {
    return TonlibError::EmptyField("account_address");
  }
  if (!request.transaction_id_) {
    return TonlibError::EmptyField("transaction_id");
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  auto lt = request.transaction_id_->lt_;
  auto hash_str = request.transaction_id_->hash_;
  if (hash_str.size() != 32) {
    return td::Status::Error(400, "Invalid transaction id hash size");
  }
  td::Bits256 hash;
  hash.as_slice().copy_from(hash_str);
  make_request(int_api::GetAccountStateByTransaction{account_address, lt, hash},
               promise.wrap([](auto&& res) { return res->to_shardAccountCell(); }));
  return td::Status::OK();
}

td::Result<ton::ManualDns::EntryData> to_dns_entry_data(tonlib_api::dns_EntryData& entry_data) {
  using R = td::Result<ton::ManualDns::EntryData>;
  return downcast_call2<R>(
      entry_data,
      td::overloaded(
          [&](tonlib_api::dns_entryDataUnknown& unknown) -> R { return ton::ManualDns::EntryData(); },
          [&](tonlib_api::dns_entryDataNextResolver& next_resolver) -> R {
            if (!next_resolver.resolver_) {
              return TonlibError::EmptyField("resolver");
            }
            TRY_RESULT(resolver, get_account_address(next_resolver.resolver_->account_address_));
            return ton::ManualDns::EntryData::next_resolver(std::move(resolver));
          },
          [&](tonlib_api::dns_entryDataSmcAddress& smc_address) -> R {
            if (!smc_address.smc_address_) {
              return TonlibError::EmptyField("smc_address");
            }
            TRY_RESULT(address, get_account_address(smc_address.smc_address_->account_address_));
            return ton::ManualDns::EntryData::smc_address(std::move(address));
          },
          [&](tonlib_api::dns_entryDataAdnlAddress& adnl_address) -> R {
            if (!adnl_address.adnl_address_) {
              return TonlibError::EmptyField("adnl_address");
            }
            TRY_RESULT(address, get_adnl_address(adnl_address.adnl_address_->adnl_address_));
            return ton::ManualDns::EntryData::adnl_address(std::move(address));
          },
          [&](tonlib_api::dns_entryDataStorageAddress& storage_address) -> R {
            return ton::ManualDns::EntryData::storage_address(storage_address.bag_id_);
          },
          [&](tonlib_api::dns_entryDataText& text) -> R { return ton::ManualDns::EntryData::text(text.text_); }));
}

class GenericCreateSendGrams : public TonlibQueryActor {
 public:
  GenericCreateSendGrams(td::actor::ActorShared<TonlibClient> client, tonlib_api::createQuery query,
                         td::optional<ton::BlockIdExt> block_id, td::Promise<td::unique_ptr<Query>>&& promise)
      : TonlibQueryActor(std::move(client))
      , query_(std::move(query))
      , promise_(std::move(promise))
      , block_id_(std::move(block_id)) {
  }

 private:
  tonlib_api::createQuery query_;
  td::Promise<td::unique_ptr<Query>> promise_;

  td::unique_ptr<AccountState> source_;
  std::vector<td::unique_ptr<AccountState>> destinations_;
  size_t destinations_left_ = 0;
  bool has_private_key_{false};
  bool is_fake_key_{false};
  td::optional<td::Ed25519::PrivateKey> private_key_;
  td::optional<td::Ed25519::PublicKey> public_key_;
  td::optional<ton::BlockIdExt> block_id_;

  struct Action {
    block::StdAddress destination;
    td::int64 amount;
    td::Ref<vm::Cell> extra_currencies;
    td::int32 send_mode{-1};

    bool is_encrypted{false};
    bool should_encrypt;
    std::string message;

    td::Ref<vm::Cell> body;
    td::Ref<vm::Cell> init_state;

    td::optional<td::Ed25519::PublicKey> o_public_key;
  };
  bool allow_send_to_uninited_{false};
  std::vector<Action> actions_;

  // We combine compelty different actions in one actor
  // Should be splitted eventually
  std::vector<ton::ManualDns::Action> dns_actions_;

  bool pchan_action_{false};

  bool rwallet_action_{false};

  void check(td::Status status) {
    if (status.is_error()) {
      promise_.set_error(std::move(status));
      return stop();
    }
  }

  void start_up() override {
    check(do_start_up());
  }
  void hangup() override {
    check(TonlibError::Cancelled());
  }

  td::Result<Action> to_action(const tonlib_api::msg_message& message) {
    if (!message.destination_) {
      return TonlibError::EmptyField("message.destination");
    }
    Action res;
    TRY_RESULT(destination, get_account_address(message.destination_->account_address_));
    res.destination = destination;
    if (message.amount_ < 0) {
      return TonlibError::InvalidField("amount", "can't be negative");
    }
    res.amount = message.amount_;
    TRY_RESULT_ASSIGN(res.extra_currencies, to_extra_currenctes_dict(message.extra_currencies_));
    if (!message.public_key_.empty()) {
      TRY_RESULT(public_key, get_public_key(message.public_key_));
      auto key = td::Ed25519::PublicKey(td::SecureString(public_key.key));
      res.o_public_key = std::move(key);
    }
    res.send_mode = message.send_mode_;
    auto status = downcast_call2<td::Status>(
        *message.data_, td::overloaded(
                            [&](tonlib_api::msg_dataRaw& text) {
                              TRY_RESULT(body, vm::std_boc_deserialize(text.body_));
                              TRY_RESULT(init_state, vm::std_boc_deserialize(text.init_state_, true));
                              res.body = std::move(body);
                              res.init_state = std::move(init_state);
                              return td::Status::OK();
                            },
                            [&](tonlib_api::msg_dataText& text) {
                              res.message = text.text_;
                              res.should_encrypt = false;
                              res.is_encrypted = false;
                              return td::Status::OK();
                            },
                            [&](tonlib_api::msg_dataDecryptedText& text) {
                              res.message = text.text_;
                              if (!has_private_key_) {
                                return TonlibError::EmptyField("input_key");
                              }
                              res.should_encrypt = true;
                              res.is_encrypted = true;
                              return td::Status::OK();
                            },
                            [&](tonlib_api::msg_dataEncryptedText& text) {
                              res.message = text.text_;
                              res.should_encrypt = false;
                              res.is_encrypted = true;
                              return td::Status::OK();
                            }));
    // Use this limit as a preventive check
    if (res.message.size() > ton::WalletV3Traits::max_message_size) {
      return TonlibError::MessageTooLong();
    }
    TRY_STATUS(std::move(status));
    return std::move(res);
  }

  td::Result<ton::ManualDns::Action> to_dns_action(tonlib_api::dns_Action& action) {
    using R = td::Result<ton::ManualDns::Action>;
    return downcast_call2<R>(action,
                             td::overloaded(
                                 [&](tonlib_api::dns_actionDeleteAll& del_all) -> R {
                                   return ton::ManualDns::Action{"", td::Bits256::zero(), {}};
                                 },
                                 [&](tonlib_api::dns_actionDelete& del) -> R {
                                   return ton::ManualDns::Action{del.name_, del.category_, {}};
                                 },
                                 [&](tonlib_api::dns_actionSet& set) -> R {
                                   if (!set.entry_) {
                                     return TonlibError::EmptyField("entry");
                                   }
                                   if (!set.entry_->entry_) {
                                     return TonlibError::EmptyField("entry.entry");
                                   }
                                   TRY_RESULT(entry_data, to_dns_entry_data(*set.entry_->entry_));
                                   TRY_RESULT(data_cell, entry_data.as_cell());
                                   return ton::ManualDns::Action{set.entry_->name_, set.entry_->category_,
                                                                 std::move(data_cell)};
                                 }));
  }

  td::Status parse_action(tonlib_api::Action& action) {
    return downcast_call2<td::Status>(action,
                                      td::overloaded([&](tonlib_api::actionNoop& cell) { return td::Status::OK(); },
                                                     [&](tonlib_api::actionMsg& cell) {
                                                       allow_send_to_uninited_ = cell.allow_send_to_uninited_;
                                                       for (auto& from_action : cell.messages_) {
                                                         if (!from_action) {
                                                           return TonlibError::EmptyField("message");
                                                         }
                                                         TRY_RESULT(action, to_action(*from_action));
                                                         actions_.push_back(std::move(action));
                                                       }
                                                       return td::Status::OK();
                                                     },
                                                     [&](tonlib_api::actionPchan& cell) {
                                                       pchan_action_ = true;
                                                       return td::Status::OK();
                                                     },
                                                     [&](tonlib_api::actionRwallet& cell) {
                                                       rwallet_action_ = true;
                                                       return td::Status::OK();
                                                     },
                                                     [&](tonlib_api::actionDns& cell) {
                                                       for (auto& from_action : cell.actions_) {
                                                         if (!from_action) {
                                                           return TonlibError::EmptyField("action");
                                                         }
                                                         TRY_RESULT(action, to_dns_action(*from_action));
                                                         dns_actions_.push_back(std::move(action));
                                                       }
                                                       return td::Status::OK();
                                                     }));
  }

  td::Status do_start_up() {
    if (query_.timeout_ < 0 || query_.timeout_ > 300) {
      return TonlibError::InvalidField("timeout", "must be between 0 and 300");
    }
    if (!query_.address_) {
      return TonlibError::EmptyField("address");
    }
    if (!query_.action_) {
      return TonlibError::EmptyField("action");
    }

    TRY_RESULT(source_address, get_account_address(query_.address_->account_address_));

    has_private_key_ = bool(query_.private_key_);
    if (has_private_key_) {
      TRY_RESULT(input_key, from_tonlib(*query_.private_key_));
      is_fake_key_ = query_.private_key_->get_id() == tonlib_api::inputKeyFake::ID;
      public_key_ = td::Ed25519::PublicKey(input_key.key.public_key.copy());
      send_query(int_api::GetPrivateKey{std::move(input_key)},
                 promise_send_closure(actor_id(this), &GenericCreateSendGrams::on_private_key));
    }
    TRY_STATUS(parse_action(*query_.action_));

    send_query(int_api::GetAccountState{source_address, block_id_.copy(), {}},
               promise_send_closure(actor_id(this), &GenericCreateSendGrams::on_source_state));

    destinations_.resize(actions_.size());
    destinations_left_ = destinations_.size();
    for (size_t i = 0; i < actions_.size(); i++) {
      send_query(int_api::GetAccountState{actions_[i].destination, block_id_.copy(), {}},
                 promise_send_closure(actor_id(this), &GenericCreateSendGrams::on_destination_state, i));
    }

    return do_loop();
  }

  void on_private_key(td::Result<KeyStorage::PrivateKey> r_key) {
    check(do_on_private_key(std::move(r_key)));
  }

  td::Status do_on_private_key(td::Result<KeyStorage::PrivateKey> r_key) {
    TRY_RESULT(key, std::move(r_key));
    private_key_ = td::Ed25519::PrivateKey(std::move(key.private_key));
    return do_loop();
  }

  void on_source_state(td::Result<td::unique_ptr<AccountState>> r_state) {
    check(do_on_source_state(std::move(r_state)));
  }

  td::Status do_on_source_state(td::Result<td::unique_ptr<AccountState>> r_state) {
    TRY_RESULT(state, std::move(r_state));
    source_ = std::move(state);
    if (source_->get_wallet_type() == AccountState::Empty && query_.initial_account_state_) {
      source_->guess_type_by_init_state(*query_.initial_account_state_);
    }
    if (source_->get_wallet_type() == AccountState::Empty && public_key_) {
      source_->guess_type_by_public_key(public_key_.value());
    }

    //TODO: pass default type through api
    if (source_->get_wallet_type() == AccountState::Empty && public_key_ && is_fake_key_) {
      source_->guess_type_default(public_key_.value());
    }

    return do_loop();
  }

  void on_destination_state(size_t i, td::Result<td::unique_ptr<AccountState>> state) {
    check(do_on_destination_state(i, std::move(state)));
  }

  td::Status do_on_destination_state(size_t i, td::Result<td::unique_ptr<AccountState>> r_state) {
    TRY_RESULT(state, std::move(r_state));
    CHECK(destinations_left_ > 0);
    destinations_left_--;
    destinations_[i] = std::move(state);
    auto& destination = *destinations_[i];
    if (destination.is_frozen()) {
      //FIXME: after restoration of frozen accounts will be supported
      return TonlibError::TransferToFrozen();
    }
    if (destination.get_wallet_type() == AccountState::Empty && destination.get_address().bounceable) {
      if (!allow_send_to_uninited_) {
        return TonlibError::DangerousTransaction("Transfer to uninited wallet");
      }
      destination.make_non_bounceable();
      LOG(INFO) << "Change destination address from bounceable to non-bounceable ";
    }
    return do_loop();
  }

  td::Status do_dns_loop() {
    if (!private_key_) {
      return TonlibError::EmptyField("private_key");
    }

    Query::Raw raw;
    auto valid_until = source_->get_sync_time();
    valid_until += query_.timeout_ == 0 ? 60 : query_.timeout_;
    raw.valid_until = valid_until;
    auto dns = ton::ManualDns::create(source_->get_smc_state());
    if (dns_actions_.empty()) {
      TRY_RESULT(message_body, dns->create_init_query(private_key_.value(), valid_until));
      raw.message_body = std::move(message_body);
    } else {
      TRY_RESULT(message_body, dns->create_update_query(private_key_.value(), dns_actions_, valid_until));
      raw.message_body = std::move(message_body);
    }
    raw.new_state = source_->get_new_state();
    raw.message = ton::GenericAccount::create_ext_message(source_->get_address(), raw.new_state, raw.message_body);
    raw.source = std::move(source_);
    raw.destinations = std::move(destinations_);
    promise_.set_value(td::make_unique<Query>(std::move(raw)));
    stop();
    return td::Status::OK();
  }

  td::Status do_pchan_loop(td::Ref<ton::PaymentChannel> pchan, tonlib_api::actionPchan& action) {
    if (!action.action_) {
      return TonlibError::EmptyField("action");
    }

    Query::Raw raw;
    auto valid_until = source_->get_sync_time();
    valid_until += query_.timeout_ == 0 ? 60 : query_.timeout_;
    raw.valid_until = valid_until;

    TRY_RESULT(info, pchan->get_info());
    bool is_alice = false;
    bool is_bob = false;
    if (info.config.a_key == private_key_.value().get_public_key().move_as_ok().as_octet_string()) {
      LOG(ERROR) << "Alice key";
      is_alice = true;
    } else if (info.config.b_key == private_key_.value().get_public_key().move_as_ok().as_octet_string()) {
      LOG(ERROR) << "Bob key";
      is_bob = true;
    }
    if (!is_alice && !is_bob) {
      return TonlibError::InvalidField("private_key", "invalid for this smartcontract");
    }
    auto status = downcast_call2<td::Status>(
        *action.action_,
        td::overloaded(
            [&](tonlib_api::pchan_actionTimeout& timeout) {
              auto builder = ton::pchan::MsgTimeoutBuilder();
              if (is_alice) {
                std::move(builder).with_a_key(&private_key_.value());
              }
              if (is_bob) {
                std::move(builder).with_b_key(&private_key_.value());
              }
              raw.message_body = std::move(builder).finalize();
              return td::Status::OK();
            },
            [&](tonlib_api::pchan_actionInit& init) {
              auto builder = ton::pchan::MsgInitBuilder()
                                 .inc_A(init.inc_A_)
                                 .inc_B(init.inc_B_)
                                 .min_A(init.min_A_)
                                 .min_B(init.min_B_)
                                 .channel_id(info.config.channel_id);
              if (is_alice) {
                std::move(builder).with_a_key(&private_key_.value());
              }
              if (is_bob) {
                std::move(builder).with_b_key(&private_key_.value());
              }
              raw.message_body = std::move(builder).finalize();
              return td::Status::OK();
            },
            [&](tonlib_api::pchan_actionClose& close) {
              if (!close.promise_) {
                return TonlibError::EmptyField("promise");
              }

              ton::pchan::SignedPromiseBuilder sbuilder;
              sbuilder.promise_A(close.promise_->promise_A_)
                  .promise_B(close.promise_->promise_B_)
                  .channel_id(close.promise_->channel_id_)
                  .signature(td::SecureString(close.promise_->signature_));
              if (is_alice && !sbuilder.check_signature(close.promise_->signature_,
                                                        td::Ed25519::PublicKey(info.config.b_key.copy()))) {
                return TonlibError::InvalidSignature();
              }
              if (is_bob && !sbuilder.check_signature(close.promise_->signature_,
                                                      td::Ed25519::PublicKey(info.config.a_key.copy()))) {
                return TonlibError::InvalidSignature();
              }

              auto builder = ton::pchan::MsgCloseBuilder()
                                 .extra_A(close.extra_A_)
                                 .extra_B(close.extra_B_)
                                 .signed_promise(sbuilder.finalize());
              if (is_alice) {
                std::move(builder).with_a_key(&private_key_.value());
              }
              if (is_bob) {
                std::move(builder).with_b_key(&private_key_.value());
              }
              raw.message_body = std::move(builder).finalize();
              return td::Status::OK();
            }));
    TRY_STATUS(std::move(status));

    raw.new_state = source_->get_new_state();
    raw.message = ton::GenericAccount::create_ext_message(source_->get_address(), raw.new_state, raw.message_body);
    raw.source = std::move(source_);

    promise_.set_value(td::make_unique<Query>(std::move(raw)));
    stop();
    return td::Status::OK();
  }
  td::Status do_pchan_loop() {
    if (!private_key_) {
      return TonlibError::EmptyField("private_key");
    }

    auto pchan = ton::PaymentChannel::create(source_->get_smc_state());

    return downcast_call2<td::Status>(
        *query_.action_, td::overloaded([&](tonlib_api::actionNoop& cell) { return td::Status::OK(); },
                                        [&](auto& cell) { return td::Status::Error(); },
                                        [&](tonlib_api::actionPchan& cell) { return do_pchan_loop(pchan, cell); }));
  }

  td::Status do_rwallet_action(td::Ref<ton::RestrictedWallet> rwallet, tonlib_api::actionRwallet& action) {
    if (!action.action_) {
      return TonlibError::EmptyField("action");
    }
    auto& init = *action.action_;
    if (!init.config_) {
      return TonlibError::EmptyField("config");
    }
    TRY_RESULT_PREFIX(start_at, td::narrow_cast_safe<td::uint32>(init.config_->start_at_),
                      TonlibError::InvalidField("start_at", "not a uint32"));
    ton::RestrictedWallet::Config config;
    config.start_at = start_at;
    for (auto& limit : init.config_->limits_) {
      if (!limit) {
        return TonlibError::EmptyField("limits");
      }
      TRY_RESULT_PREFIX(seconds, td::narrow_cast_safe<td::int32>(limit->seconds_),
                        TonlibError::InvalidField("seconds", "not a int32"));
      TRY_RESULT_PREFIX(value, td::narrow_cast_safe<td::uint64>(limit->value_),
                        TonlibError::InvalidField("value", "not a uint64"));
      config.limits.emplace_back(seconds, value);
    }
    Query::Raw raw;
    auto valid_until = source_->get_sync_time();
    valid_until += query_.timeout_ == 0 ? 60 : query_.timeout_;
    raw.valid_until = valid_until;

    TRY_RESULT_PREFIX(message_body, rwallet->get_init_message(private_key_.value(), valid_until, config),
                      TonlibError::Internal("Invalid rwalet init query"));
    raw.message_body = std::move(message_body);
    raw.new_state = source_->get_new_state();
    raw.message = ton::GenericAccount::create_ext_message(source_->get_address(), raw.new_state, raw.message_body);
    raw.source = std::move(source_);
    raw.destinations = std::move(destinations_);
    promise_.set_value(td::make_unique<Query>(std::move(raw)));
    stop();
    return td::Status::OK();
  }

  td::Status do_rwallet_action() {
    if (!private_key_) {
      return TonlibError::EmptyField("private_key");
    }
    auto rwallet = ton::RestrictedWallet::create(source_->get_smc_state());
    return downcast_call2<td::Status>(
        *query_.action_,
        td::overloaded([&](auto& cell) { return td::Status::Error("UNREACHABLE"); },
                       [&](tonlib_api::actionRwallet& cell) { return do_rwallet_action(rwallet, cell); }));
  }

  td::Status do_loop() {
    if (!source_ || destinations_left_ != 0) {
      return td::Status::OK();
    }
    if (has_private_key_ && !private_key_) {
      return td::Status::OK();
    }

    if (source_->get_wallet_type() == AccountState::ManualDns) {
      return do_dns_loop();
    }
    if (source_->get_wallet_type() == AccountState::PaymentChannel) {
      return do_pchan_loop();
    }
    if (rwallet_action_ && source_->get_wallet_type() == AccountState::RestrictedWallet) {
      return do_rwallet_action();
    }

    switch (source_->get_wallet_type()) {
      case AccountState::Empty:
        return TonlibError::AccountNotInited();
      case AccountState::Unknown:
        return TonlibError::AccountTypeUnknown();
      default:
        break;
    }

    if (!source_->is_wallet()) {
      return TonlibError::AccountActionUnsupported("wallet action");
    }

    td::int64 amount = 0;
    td::Ref<vm::Cell> extra_currencies;
    for (auto& action : actions_) {
      amount += action.amount;
      TRY_RESULT_ASSIGN(extra_currencies, add_extra_currencies(extra_currencies, action.extra_currencies));
    }

    if (amount > source_->get_balance()) {
      return TonlibError::NotEnoughFunds();
    }

    // Temporary turn off this dangerous transfer
    if (amount == source_->get_balance()) {
      return TonlibError::NotEnoughFunds();
    }

    TRY_STATUS(check_enough_extra_currencies(source_->get_extra_currencies(), extra_currencies));

    if (source_->get_wallet_type() == AccountState::RestrictedWallet) {
      auto r_unlocked_balance = ton::RestrictedWallet::create(source_->get_smc_state())
                                    ->get_balance(source_->get_balance(), source_->get_sync_time());
      if (r_unlocked_balance.is_ok() && amount > static_cast<td::int64>(r_unlocked_balance.ok())) {
        return TonlibError::NotEnoughFunds();
      }
    }

    auto valid_until = source_->get_sync_time();
    valid_until += query_.timeout_ == 0 ? 60 : query_.timeout_;
    std::vector<ton::WalletInterface::Gift> gifts;
    size_t i = 0;
    for (auto& action : actions_) {
      ton::HighloadWalletV2::Gift gift;
      auto& destination = destinations_[i];
      gift.destination = destinations_[i]->get_address();
      gift.gramms = action.amount;
      gift.extra_currencies = action.extra_currencies;
      gift.send_mode = action.send_mode;

      // Temporary turn off this dangerous transfer
      // if (action.amount == source_->get_balance()) {
      //   gift.gramms = -1;
      // }

      if (action.body.not_null()) {
        gift.body = action.body;
        gift.init_state = action.init_state;
      } else if (action.should_encrypt) {
        LOG(ERROR) << "TRY ENCRYPT";
        if (!private_key_) {
          return TonlibError::EmptyField("private_key");
        }

        auto o_public_key = std::move(action.o_public_key);
        if (!o_public_key && destination->is_wallet()) {
          auto wallet = destination->get_wallet();
          auto r_public_key = wallet->get_public_key();
          if (r_public_key.is_ok()) {
            o_public_key = r_public_key.move_as_ok();
          }
        }

        if (!o_public_key) {
          auto smc = ton::SmartContract::create(destination->get_smc_state());
          auto r_public_key = ton::GenericAccount::get_public_key(destination->get_smc_state());
          if (r_public_key.is_ok()) {
            o_public_key = r_public_key.move_as_ok();
          }
        }

        if (!o_public_key) {
          return TonlibError::MessageEncryption("Cannot get public key of destination (possibly unknown wallet type)");
        }

        auto addr = source_->get_address();
        addr.bounceable = true;
        addr.testnet = false;

        TRY_RESULT_PREFIX(encrypted_message,
                          SimpleEncryptionV2::encrypt_data(action.message, o_public_key.unwrap(), private_key_.value(),
                                                           addr.rserialize(true)),
                          TonlibError::Internal());
        gift.message = encrypted_message.as_slice().str();
        gift.is_encrypted = true;
      } else {
        gift.message = action.message;
        gift.is_encrypted = action.is_encrypted;
      }
      i++;
      gifts.push_back(gift);
    }

    Query::Raw raw;
    auto with_wallet = [&](auto&& wallet) {
      if (!private_key_) {
        return TonlibError::EmptyField("private_key");
      }
      if (wallet.get_max_gifts_size() < gifts.size()) {
        return TonlibError::MessageTooLong();  // TODO: other error
      }

      raw.valid_until = valid_until;
      TRY_RESULT(message_body, wallet.make_a_gift_message(private_key_.unwrap(), valid_until, gifts));
      raw.message_body = std::move(message_body);
      raw.new_state = source_->get_new_state();
      raw.message = ton::GenericAccount::create_ext_message(source_->get_address(), raw.new_state, raw.message_body);
      raw.source = std::move(source_);
      raw.destinations = std::move(destinations_);

      promise_.set_value(td::make_unique<Query>(std::move(raw)));
      stop();
      return td::Status::OK();
    };

    return with_wallet(*source_->get_wallet());
  }
};

td::int64 TonlibClient::register_query(td::unique_ptr<Query> query) {
  auto query_id = ++next_query_id_;
  queries_[query_id] = std::move(query);
  return query_id;
}

td::Result<tonlib_api::object_ptr<tonlib_api::query_info>> TonlibClient::get_query_info(td::int64 id) {
  auto it = queries_.find(id);
  if (it == queries_.end()) {
    return TonlibError::InvalidQueryId();
  }
  return tonlib_api::make_object<tonlib_api::query_info>(
      id, it->second->get_valid_until(), it->second->get_body_hash().as_slice().str(),
      to_bytes(it->second->get_message_body()), to_bytes(it->second->get_init_state()));
}

void TonlibClient::finish_create_query(td::Result<td::unique_ptr<Query>> r_query,
                                       td::Promise<object_ptr<tonlib_api::query_info>>&& promise) {
  TRY_RESULT_PROMISE(promise, query, std::move(r_query));
  auto id = register_query(std::move(query));
  promise.set_result(get_query_info(id));
}

td::Status TonlibClient::do_request(tonlib_api::createQuery& request,
                                    td::Promise<object_ptr<tonlib_api::query_info>>&& promise) {
  auto id = actor_id_++;
  actors_[id] = td::actor::create_actor<GenericCreateSendGrams>(
      "GenericSendGrams", actor_shared(this, id), std::move(request), query_context_.block_id.copy(),
      promise.send_closure(actor_id(this), &TonlibClient::finish_create_query));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(tonlib_api::msg_decrypt& request,
                                    td::Promise<object_ptr<tonlib_api::msg_dataDecryptedArray>>&& promise) {
  if (!request.input_key_) {
    return TonlibError::EmptyField("input_key");
  }
  if (!request.data_) {
    return TonlibError::EmptyField("data");
  }
  TRY_RESULT(input_key, from_tonlib(*request.input_key_));
  using ReturnType = tonlib_api::object_ptr<tonlib_api::msg_dataDecrypted>;
  make_request(
      int_api::GetPrivateKey{std::move(input_key)},
      promise.wrap([elements = std::move(request.data_)](auto key) mutable {
        auto private_key = td::Ed25519::PrivateKey(std::move(key.private_key));
        auto new_elements = td::transform(std::move(elements->elements_), [&private_key](auto msg) -> ReturnType {
          auto res = tonlib_api::make_object<tonlib_api::msg_dataDecrypted>();
          if (!msg) {
            return res;
          }
          if (!msg->data_) {
            return res;
          }
          res->data_ = std::move(msg->data_);
          if (!msg->source_) {
            return res;
          }
          auto r_account_address = get_account_address(msg->source_->account_address_);
          if (r_account_address.is_error()) {
            return res;
          }
          return downcast_call2<ReturnType>(
              *res->data_,
              td::overloaded(
                  [&res](auto&) { return std::move(res); },
                  [&res, &private_key, &msg](tonlib_api::msg_dataEncryptedText& encrypted) -> ReturnType {
                    auto r_decrypted =
                        SimpleEncryptionV2::decrypt_data(encrypted.text_, private_key, msg->source_->account_address_);
                    if (r_decrypted.is_error()) {
                      return std::move(res);
                    }
                    auto decrypted = r_decrypted.move_as_ok();
                    return tonlib_api::make_object<tonlib_api::msg_dataDecrypted>(
                        decrypted.proof.as_slice().str(),
                        tonlib_api::make_object<tonlib_api::msg_dataDecryptedText>(decrypted.data.as_slice().str()));
                  }));
        });
        return tonlib_api::make_object<tonlib_api::msg_dataDecryptedArray>(std::move(new_elements));
      }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::raw_createQuery& request,
                                    td::Promise<object_ptr<tonlib_api::query_info>>&& promise) {
  if (!request.destination_) {
    return TonlibError::EmptyField("destination");
  }
  TRY_RESULT(account_address, get_account_address(request.destination_->account_address_));

  td::optional<ton::SmartContract::State> smc_state;
  if (!request.init_code_.empty()) {
    TRY_RESULT_PREFIX(code, vm::std_boc_deserialize(request.init_code_), TonlibError::InvalidBagOfCells("init_code"));
    TRY_RESULT_PREFIX(data, vm::std_boc_deserialize(request.init_data_), TonlibError::InvalidBagOfCells("init_data"));
    smc_state = ton::SmartContract::State{std::move(code), std::move(data)};
  }
  TRY_RESULT_PREFIX(body, vm::std_boc_deserialize(request.body_), TonlibError::InvalidBagOfCells("body"));

  td::Promise<td::unique_ptr<Query>> new_promise =
      promise.send_closure(actor_id(this), &TonlibClient::finish_create_query);

  make_request(int_api::GetAccountState{account_address, query_context_.block_id.copy(), {}},
               new_promise.wrap([smc_state = std::move(smc_state), body = std::move(body)](auto&& source) mutable {
                 Query::Raw raw;
                 if (smc_state) {
                   source->set_new_state(smc_state.unwrap());
                 }
                 raw.new_state = source->get_new_state();
                 raw.message_body = std::move(body);
                 raw.message =
                     ton::GenericAccount::create_ext_message(source->get_address(), raw.new_state, raw.message_body);
                 raw.source = std::move(source);
                 return td::make_unique<Query>(std::move(raw));
               }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::query_getInfo& request,
                                    td::Promise<object_ptr<tonlib_api::query_info>>&& promise) {
  promise.set_result(get_query_info(request.id_));
  return td::Status::OK();
}

void TonlibClient::query_estimate_fees(td::int64 id, bool ignore_chksig, td::Result<LastConfigState> r_state,
                                       td::Promise<object_ptr<tonlib_api::query_fees>>&& promise) {
  auto it = queries_.find(id);
  if (it == queries_.end()) {
    promise.set_error(TonlibError::InvalidQueryId());
    return;
  }
  TRY_RESULT_PROMISE(promise, state, std::move(r_state));
  TRY_RESULT_PROMISE_PREFIX(promise, fees, TRY_VM(it->second->estimate_fees(ignore_chksig, state, libraries)),
                            TonlibError::Internal());
  promise.set_value(tonlib_api::make_object<tonlib_api::query_fees>(
      fees.first.to_tonlib_api(), td::transform(fees.second, [](auto& x) { return x.to_tonlib_api(); })));
}

td::Status TonlibClient::do_request(const tonlib_api::query_estimateFees& request,
                                    td::Promise<object_ptr<tonlib_api::query_fees>>&& promise) {
  auto it = queries_.find(request.id_);
  if (it == queries_.end()) {
    return TonlibError::InvalidQueryId();
  }

  client_.with_last_config([this, id = request.id_, ignore_chksig = request.ignore_chksig_,
                            promise = std::move(promise)](td::Result<LastConfigState> r_state) mutable {
    this->query_estimate_fees(id, ignore_chksig, std::move(r_state), std::move(promise));
  });
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::query_send& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  auto it = queries_.find(request.id_);
  if (it == queries_.end()) {
    return TonlibError::InvalidQueryId();
  }

  auto message = it->second->get_message();
  if (GET_VERBOSITY_LEVEL() >= VERBOSITY_NAME(DEBUG)) {
    std::ostringstream ss;
    block::gen::t_Message_Any.print_ref(ss, message);
    LOG(DEBUG) << ss.str();
  }
  make_request(int_api::SendMessage{std::move(message)}, to_any_promise(std::move(promise)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(tonlib_api::query_forget& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  if (queries_.erase(request.id_) == 0) {
    return TonlibError::InvalidQueryId();
  }
  promise.set_value(tonlib_api::make_object<tonlib_api::ok>());
  return td::Status::OK();
}

td::int64 TonlibClient::register_smc(td::unique_ptr<AccountState> smc) {
  auto smc_id = ++next_smc_id_;
  smcs_[smc_id] = std::move(smc);
  return smc_id;
}

td::Result<tonlib_api::object_ptr<tonlib_api::smc_info>> TonlibClient::get_smc_info(td::int64 id) {
  auto it = smcs_.find(id);
  if (it == smcs_.end()) {
    return TonlibError::InvalidSmcId();
  }
  return tonlib_api::make_object<tonlib_api::smc_info>(id);
}

void TonlibClient::finish_load_smc(td::unique_ptr<AccountState> smc,
                                   td::Promise<object_ptr<tonlib_api::smc_info>>&& promise) {
  auto id = register_smc(std::move(smc));
  promise.set_result(get_smc_info(id));
}

td::Status TonlibClient::do_request(const tonlib_api::smc_load& request,
                                    td::Promise<object_ptr<tonlib_api::smc_info>>&& promise) {
  if (!request.account_address_) {
    return TonlibError::EmptyField("account_address");
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  make_request(int_api::GetAccountState{std::move(account_address), query_context_.block_id.copy(), {}},
               promise.send_closure(actor_id(this), &TonlibClient::finish_load_smc));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::smc_loadByTransaction& request,
                                    td::Promise<object_ptr<tonlib_api::smc_info>>&& promise) {
  if (!request.account_address_) {
    return TonlibError::EmptyField("account_address");
  }
  if (!request.transaction_id_) {
    return TonlibError::EmptyField("transaction_id");
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  auto lt = request.transaction_id_->lt_;
  auto hash_str = request.transaction_id_->hash_;
  if (hash_str.size() != 32) {
    return td::Status::Error(400, "Invalid transaction id hash size");
  }
  td::Bits256 hash;
  hash.as_slice().copy_from(hash_str);
  make_request(int_api::GetAccountStateByTransaction{account_address, lt, hash},
               promise.send_closure(actor_id(this), &TonlibClient::finish_load_smc));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::smc_forget& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  auto it = smcs_.find(request.id_);
  if (it == smcs_.end()) {
    return TonlibError::InvalidSmcId();
  }
  smcs_.erase(it);
  promise.set_value(tonlib_api::make_object<tonlib_api::ok>());
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::smc_getCode& request,
                                    td::Promise<object_ptr<tonlib_api::tvm_cell>>&& promise) {
  auto it = smcs_.find(request.id_);
  if (it == smcs_.end()) {
    return TonlibError::InvalidSmcId();
  }
  auto& acc = it->second;
  auto code = acc->get_smc_state().code;
  promise.set_value(tonlib_api::make_object<tonlib_api::tvm_cell>(to_bytes(code)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::smc_getData& request,
                                    td::Promise<object_ptr<tonlib_api::tvm_cell>>&& promise) {
  auto it = smcs_.find(request.id_);
  if (it == smcs_.end()) {
    return TonlibError::InvalidSmcId();
  }
  auto& acc = it->second;
  auto data = acc->get_smc_state().data;
  promise.set_value(tonlib_api::make_object<tonlib_api::tvm_cell>(to_bytes(data)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::smc_getState& request,
                                    td::Promise<object_ptr<tonlib_api::tvm_cell>>&& promise) {
  auto it = smcs_.find(request.id_);
  if (it == smcs_.end()) {
    return TonlibError::InvalidSmcId();
  }
  auto& acc = it->second;
  auto data = acc->get_raw_state();
  promise.set_value(tonlib_api::make_object<tonlib_api::tvm_cell>(to_bytes(data)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::smc_getRawFullAccountState& request,
                                    td::Promise<object_ptr<tonlib_api::raw_fullAccountState>>&& promise) {
  auto it = smcs_.find(request.id_);
  if (it == smcs_.end()) {
    return TonlibError::InvalidSmcId();
  }
  auto& acc = it->second;
  promise.set_result(acc->to_raw_fullAccountState());
  return td::Status::OK();
}

bool is_list(vm::StackEntry entry) {
  while (true) {
    if (entry.type() == vm::StackEntry::Type::t_null) {
      return true;
    }
    if (entry.type() != vm::StackEntry::Type::t_tuple) {
      return false;
    }
    if (entry.as_tuple()->size() != 2) {
      return false;
    }
    entry = entry.as_tuple()->at(1);
  }
};
auto to_tonlib_api(const vm::StackEntry& entry, int& limit) -> td::Result<tonlib_api::object_ptr<tonlib_api::tvm_StackEntry>> {
  if (limit <= 0) {
    return td::Status::Error(PSLICE() << "TVM stack size exceeds limit");
  }
  switch (entry.type()) {
    case vm::StackEntry::Type::t_int:
      return tonlib_api::make_object<tonlib_api::tvm_stackEntryNumber>(
          tonlib_api::make_object<tonlib_api::tvm_numberDecimal>(dec_string(entry.as_int())));
    case vm::StackEntry::Type::t_slice:
      return tonlib_api::make_object<tonlib_api::tvm_stackEntrySlice>(tonlib_api::make_object<tonlib_api::tvm_slice>(
          to_bytes(vm::CellBuilder().append_cellslice(entry.as_slice()).finalize())));
    case vm::StackEntry::Type::t_cell:
      return tonlib_api::make_object<tonlib_api::tvm_stackEntryCell>(
          tonlib_api::make_object<tonlib_api::tvm_cell>(to_bytes(entry.as_cell())));
    case vm::StackEntry::Type::t_null:
    case vm::StackEntry::Type::t_tuple: {
      std::vector<tonlib_api::object_ptr<tonlib_api::tvm_StackEntry>> elements;
      if (is_list(entry)) {
        auto node = entry;
        while (node.type() == vm::StackEntry::Type::t_tuple) {
          TRY_RESULT(tl_entry, to_tonlib_api(node.as_tuple()->at(0), --limit));
          elements.push_back(std::move(tl_entry));
          node = node.as_tuple()->at(1);
        }
        return tonlib_api::make_object<tonlib_api::tvm_stackEntryList>(
            tonlib_api::make_object<tonlib_api::tvm_list>(std::move(elements)));

      } else {
        for (auto& element : *entry.as_tuple()) {
          TRY_RESULT(tl_entry, to_tonlib_api(element, --limit));
          elements.push_back(std::move(tl_entry));
        }
        return tonlib_api::make_object<tonlib_api::tvm_stackEntryTuple>(
            tonlib_api::make_object<tonlib_api::tvm_tuple>(std::move(elements)));
      }
    }

    default:
      return tonlib_api::make_object<tonlib_api::tvm_stackEntryUnsupported>();
  }
};

auto to_tonlib_api(const td::Ref<vm::Stack>& stack) -> td::Result<std::vector<tonlib_api::object_ptr<tonlib_api::tvm_StackEntry>>> {
  int stack_limit = 8000;
  std::vector<tonlib_api::object_ptr<tonlib_api::tvm_StackEntry>> tl_stack;
  for (auto& entry: stack->as_span()) {
    TRY_RESULT(tl_entry, to_tonlib_api(entry, --stack_limit));
    tl_stack.push_back(std::move(tl_entry));
  }
  return tl_stack;
}

td::Result<vm::StackEntry> from_tonlib_api(tonlib_api::tvm_StackEntry& entry) {
  // TODO: error codes
  // downcast_call
  return downcast_call2<td::Result<vm::StackEntry>>(
      entry,
      td::overloaded(
          [&](tonlib_api::tvm_stackEntryUnsupported& cell) { return td::Status::Error("Unsuppored stack entry"); },
          [&](tonlib_api::tvm_stackEntrySlice& cell) -> td::Result<vm::StackEntry> {
            TRY_RESULT(res, vm::std_boc_deserialize(cell.slice_->bytes_));
            auto slice = vm::load_cell_slice_ref(std::move(res));
            return vm::StackEntry{std::move(slice)};
          },
          [&](tonlib_api::tvm_stackEntryCell& cell) -> td::Result<vm::StackEntry> {
            TRY_RESULT(res, vm::std_boc_deserialize(cell.cell_->bytes_));
            return vm::StackEntry{std::move(res)};
          },
          [&](tonlib_api::tvm_stackEntryTuple& tuple) -> td::Result<vm::StackEntry> {
            std::vector<vm::StackEntry> elements;
            for (auto& element : tuple.tuple_->elements_) {
              TRY_RESULT(new_element, from_tonlib_api(*element));
              elements.push_back(std::move(new_element));
            }
            return td::Ref<vm::Tuple>(true, std::move(elements));
          },
          [&](tonlib_api::tvm_stackEntryList& tuple) -> td::Result<vm::StackEntry> {
            vm::StackEntry tail;
            for (auto& element : td::reversed(tuple.list_->elements_)) {
              TRY_RESULT(new_element, from_tonlib_api(*element));
              tail = vm::make_tuple_ref(std::move(new_element), std::move(tail));
            }
            return tail;
          },
          [&](tonlib_api::tvm_stackEntryNumber& number) -> td::Result<vm::StackEntry> {
            auto& dec = *number.number_;
            auto num = td::dec_string_to_int256(dec.number_);
            if (num.is_null()) {
              return td::Status::Error("Failed to parse dec string to int256");
            }
            return num;
          }));
}

void deep_library_search(std::set<td::Bits256>& set, std::set<vm::Cell::Hash>& visited,
                         vm::Dictionary& libs, td::Ref<vm::Cell> cell, int depth, size_t max_libs = 16) {
  if (depth <= 0 || set.size() >= max_libs || visited.size() >= 256) {
    return;
  }
  auto ins = visited.insert(cell->get_hash());
  if (!ins.second) {
    return;  // already visited this cell
  }
  auto r_loaded_cell = cell->load_cell();
  if (r_loaded_cell.is_error()) {
    return;
  }
  auto loaded_cell = r_loaded_cell.move_as_ok();
  if (loaded_cell.data_cell->is_special()) {
    if (loaded_cell.data_cell->special_type() == vm::DataCell::SpecialType::Library) {
      vm::CellSlice cs(std::move(loaded_cell));
      if (cs.size() != vm::Cell::hash_bits + 8) {
        return;
      }
      auto key = td::Bits256(cs.data_bits() + 8);
      if (libs.lookup(key).is_null()) {
        set.insert(key);
      }
    }
    return;
  }
  for (unsigned int i=0; i<loaded_cell.data_cell->get_refs_cnt(); i++) {
    deep_library_search(set, visited, libs, loaded_cell.data_cell->get_ref(i), depth - 1, max_libs);
  }
}

td::Status TonlibClient::do_request(const tonlib_api::smc_getLibraries& request,
                                    td::Promise<object_ptr<tonlib_api::smc_libraryResult>>&& promise) {
  if (request.library_list_.size() > 16) {
    promise.set_error(TonlibError::InvalidField("library_list", ": too many libraries requested, 16 maximum"));
  }
  if (query_context_.block_id) {
    get_libraries(query_context_.block_id.value(), request.library_list_, std::move(promise));
  } else {
    client_.with_last_block([this, promise = std::move(promise), library_list = request.library_list_](td::Result<LastBlockState> r_last_block) mutable {
      if (r_last_block.is_error()) {
        promise.set_error(r_last_block.move_as_error_prefix(TonlibError::Internal("get last block failed ")));
      } else {
        this->get_libraries(r_last_block.move_as_ok().last_block_id, library_list, std::move(promise));
      }
    });
  }
  return td::Status::OK();
}

void TonlibClient::get_libraries(ton::BlockIdExt blkid, std::vector<td::Bits256> library_list, td::Promise<object_ptr<tonlib_api::smc_libraryResult>>&& promise) {
  sort(library_list.begin(), library_list.end());
  library_list.erase(unique(library_list.begin(), library_list.end()), library_list.end());

  std::vector<object_ptr<tonlib_api::smc_libraryEntry>> result_entries;
  result_entries.reserve(library_list.size());
  std::vector<td::Bits256> not_cached_hashes;
  not_cached_hashes.reserve(library_list.size());

  for (auto& library_hash : library_list) {
    if (libraries.key_exists(library_hash)) {
      auto library_content = vm::std_boc_serialize(libraries.lookup_ref(library_hash)).move_as_ok().as_slice().str();
      result_entries.push_back(tonlib_api::make_object<tonlib_api::smc_libraryEntry>(library_hash, library_content));
    } else {
      not_cached_hashes.push_back(library_hash);
    }
  }

  if (not_cached_hashes.empty()) {
    promise.set_value(tonlib_api::make_object<tonlib_api::smc_libraryResult>(std::move(result_entries)));
    return;
  }

  auto missed_lib_ids = not_cached_hashes;
  client_.send_query(ton::lite_api::liteServer_getLibrariesWithProof(ton::create_tl_lite_block_id(blkid), 1, std::move(missed_lib_ids)),
                     promise.wrap([self=this, blkid, result_entries = std::move(result_entries), not_cached_hashes]
                                  (td::Result<ton::lite_api::object_ptr<ton::lite_api::liteServer_libraryResultWithProof>> r_libraries) mutable
                                    -> td::Result<tonlib_api::object_ptr<tonlib_api::smc_libraryResult>> {
    if (r_libraries.is_error()) {
      LOG(WARNING) << "cannot obtain found libraries: " << r_libraries.move_as_error().to_string();
      return r_libraries.move_as_error();
    }

    auto libraries = r_libraries.move_as_ok();
    auto state = block::check_extract_state_proof(blkid, libraries->state_proof_.as_slice(),
                                              libraries->data_proof_.as_slice());
    if (state.is_error()) {
      LOG(WARNING) << "cannot check state proof: " << state.move_as_error().to_string();
      return state.move_as_error();
    }
    auto state_root = state.move_as_ok();

    try {
      block::gen::ShardStateUnsplit::Record state_record;
      if (!tlb::unpack_cell(state_root, state_record)) {
        return td::Status::Error("cannot unpack shardchain state");
      }
      auto libraries_dict = vm::Dictionary(state_record.r1.libraries->prefetch_ref(), 256);

      for (auto& hash : not_cached_hashes) {
        auto csr = libraries_dict.lookup(hash.bits(), 256);
        if (csr.is_null()) {
          LOG(WARNING) << "library " << hash.to_hex() << " not found in config";
          if (std::any_of(libraries->result_.begin(), libraries->result_.end(),
                        [&hash](const auto& lib) { return lib->hash_.bits().equals(hash.cbits(), 256); })) {
            return TonlibError::Internal("library is included in response but it's not found in proof");
          }
          continue;
        }
        block::gen::LibDescr::Record libdescr;
        if (!tlb::csr_unpack(csr, libdescr)) {
          return TonlibError::Internal("cannot unpack LibDescr record");
        }

        auto lib_it = std::find_if(libraries->result_.begin(), libraries->result_.end(),
                                [&hash](const auto& lib) { return lib->hash_.bits().equals(hash.cbits(), 256); });
        if (lib_it == libraries->result_.end()) {
          return TonlibError::Internal("library is found in proof but not in response");
        }
        auto& lib = *lib_it;
        auto contents = vm::std_boc_deserialize(lib->data_);
        if (!contents.is_ok() || contents.ok().is_null()) {
           return TonlibError::Internal(PSLICE() << "cannot deserialize library cell " << lib->hash_.to_hex());
        }

        if (!contents.ok()->get_hash().bits().equals(hash.cbits(), 256)) {
            return TonlibError::Internal(PSLICE() << "library hash mismatch data " << contents.ok()->get_hash().to_hex() << " != requested " << hash.to_hex());
        }

        if (contents.ok()->get_hash() != libdescr.lib->get_hash()) {
          return TonlibError::Internal(PSLICE() << "library hash mismatch data " << lib->hash_.to_hex() << " != proof " << libdescr.lib->get_hash().to_hex());
        }

        result_entries.push_back(tonlib_api::make_object<tonlib_api::smc_libraryEntry>(lib->hash_, lib->data_.as_slice().str()));
        self->libraries.set_ref(lib->hash_, contents.move_as_ok());
        LOG(DEBUG) << "registered library " << lib->hash_.to_hex();
      }
      self->store_libs_to_disk();
      return tonlib_api::make_object<tonlib_api::smc_libraryResult>(std::move(result_entries));
    } catch (vm::VmError& err) {
      return TonlibError::Internal(PSLICE() << "error while checking getLibrariesWithProof proof: " << err.get_msg());
    } catch (vm::VmVirtError& err) {
      return TonlibError::Internal(PSLICE() << "virtualization error while checking getLibrariesWithProof proof: " << err.get_msg());
    }
  }));
}

td::Status TonlibClient::do_request(const tonlib_api::smc_getLibrariesExt& request,
                                    td::Promise<object_ptr<tonlib_api::smc_libraryResultExt>>&& promise) {
  std::set<td::Bits256> request_libs;
  for (auto& x : request.list_) {
    td::Status status = td::Status::OK();
    downcast_call(*x, td::overloaded([&](tonlib_api::smc_libraryQueryExt_one& one) { request_libs.insert(one.hash_); },
                                     [&](tonlib_api::smc_libraryQueryExt_scanBoc& scan) {
                                       std::set<vm::Cell::Hash> visited;
                                       vm::Dictionary empty{256};
                                       td::Result<td::Ref<vm::Cell>> r_cell = vm::std_boc_deserialize(scan.boc_);
                                       if (r_cell.is_error()) {
                                         status = r_cell.move_as_error();
                                         return;
                                       }
                                       size_t max_libs = scan.max_libs_ < 0 ? (1 << 30) : (size_t)scan.max_libs_;
                                       std::set<td::Bits256> new_libs;
                                       deep_library_search(new_libs, visited, empty, r_cell.move_as_ok(), 1024,
                                                           max_libs);
                                       request_libs.insert(new_libs.begin(), new_libs.end());
                                     }));
    TRY_STATUS(std::move(status));
  }
  std::vector<td::Bits256> not_cached;
  for (const td::Bits256& h : request_libs) {
    if (libraries.lookup(h).is_null()) {
      not_cached.push_back(h);
    }
  }
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  LOG(DEBUG) << "Requesting " << not_cached.size() << " libraries";
  for (size_t i = 0; i < not_cached.size(); i += 16) {
    size_t r = std::min(i + 16, not_cached.size());
    client_.send_query(
        ton::lite_api::liteServer_getLibraries(
            std::vector<td::Bits256>(not_cached.begin() + i, not_cached.begin() + r)),
        [self = this, promise = ig.get_promise()](
            td::Result<ton::lite_api::object_ptr<ton::lite_api::liteServer_libraryResult>> r_libraries) mutable {
          self->process_new_libraries(std::move(r_libraries));
          promise.set_result(td::Unit());
        });
  }

  ig.add_promise(promise.wrap([self = this, libs = std::move(request_libs)](td::Unit&&) {
    vm::Dictionary dict{256};
    std::vector<td::Bits256> libs_ok, libs_not_found;
    for (const auto& h : libs) {
      auto lib = self->libraries.lookup_ref(h);
      if (lib.is_null()) {
        libs_not_found.push_back(h);
      } else {
        libs_ok.push_back(h);
        dict.set_ref(h, lib);
      }
    }
    td::BufferSlice dict_boc;
    if (!dict.is_empty()) {
      dict_boc = vm::std_boc_serialize(dict.get_root_cell()).move_as_ok();
    }
    return ton::create_tl_object<tonlib_api::smc_libraryResultExt>(dict_boc.as_slice().str(), std::move(libs_ok),
                                                                   std::move(libs_not_found));
  }));

  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::smc_runGetMethod& request,
                                    td::Promise<object_ptr<tonlib_api::smc_runResult>>&& promise) {
  auto it = smcs_.find(request.id_);
  if (it == smcs_.end()) {
    return TonlibError::InvalidSmcId();
  }

  td::Ref<ton::SmartContract> smc(true, it->second->get_smc_state());
  ton::SmartContract::Args args;
  downcast_call(*request.method_,
                td::overloaded([&](tonlib_api::smc_methodIdNumber& number) { args.set_method_id(number.number_); },
                               [&](tonlib_api::smc_methodIdName& name) { args.set_method_id(name.name_); }));
  td::Ref<vm::Stack> stack(true);
  td::Status status;
  for (auto& entry : request.stack_) {
    TRY_RESULT(e, from_tonlib_api(*entry));
    stack.write().push(std::move(e));
  }
  args.set_stack(std::move(stack));
  args.set_balance(it->second->get_balance());
  args.set_extra_currencies(it->second->get_extra_currencies());
  args.set_now(it->second->get_sync_time());
  args.set_address(it->second->get_address());

  client_.with_last_config([self = this, smc = std::move(smc), args = std::move(args), promise = std::move(promise)
  ](td::Result<LastConfigState> r_state) mutable {
    TRY_RESULT_PROMISE(promise, state, std::move(r_state));
    args.set_config(state.config);
    args.set_prev_blocks_info(state.prev_blocks_info);

    auto code = smc->get_state().code;
    if (code.not_null()) {
      std::set<td::Bits256> librarySet;
      std::set<vm::Cell::Hash> visited;
      deep_library_search(librarySet, visited, self->libraries, code, 24);
      std::vector<td::Bits256> libraryList{librarySet.begin(), librarySet.end()};
      if (libraryList.size() > 0) {
        LOG(DEBUG) << "Requesting found libraries in code (" << libraryList.size() << ")";
        self->client_.send_query(
            ton::lite_api::liteServer_getLibraries(std::move(libraryList)),
            [self, smc = std::move(smc), args = std::move(args), promise = std::move(promise)](
                td::Result<ton::lite_api::object_ptr<ton::lite_api::liteServer_libraryResult>> r_libraries) mutable {
              self->process_new_libraries(std::move(r_libraries));
              self->perform_smc_execution(std::move(smc), std::move(args), std::move(promise));
            });
      } else {
        self->perform_smc_execution(std::move(smc), std::move(args), std::move(promise));
      }
    }
    else {
      self->perform_smc_execution(std::move(smc), std::move(args), std::move(promise));
    }
  });
  return td::Status::OK();
}

void TonlibClient::process_new_libraries(
    td::Result<ton::lite_api::object_ptr<ton::lite_api::liteServer_libraryResult>> r_libraries) {
  if (r_libraries.is_error()) {
    LOG(WARNING) << "cannot obtain found libraries: " << r_libraries.move_as_error().to_string();
  } else {
    auto new_libraries = r_libraries.move_as_ok();
    bool updated = false;
    for (auto& lr : new_libraries->result_) {
      auto contents = vm::std_boc_deserialize(lr->data_);
      if (contents.is_ok() && contents.ok().not_null()) {
        if (contents.ok()->get_hash().bits().compare(lr->hash_.cbits(), 256)) {
          LOG(WARNING) << "hash mismatch for library " << lr->hash_.to_hex();
          continue;
        }
        libraries.set_ref(lr->hash_, contents.move_as_ok());
        updated = true;
        LOG(DEBUG) << "registered library " << lr->hash_.to_hex();
      } else {
        LOG(WARNING) << "failed to deserialize library: " << lr->hash_.to_hex();
      }
    }
    if (updated) {
      store_libs_to_disk();
    }
  }
}

void TonlibClient::perform_smc_execution(td::Ref<ton::SmartContract> smc, ton::SmartContract::Args args,
                                         td::Promise<object_ptr<tonlib_api::smc_runResult>>&& promise) {

  args.set_libraries(libraries);

  auto res = smc->run_get_method(args);

  // smc.runResult gas_used:int53 stack:vector<tvm.StackEntry> exit_code:int32 = smc.RunResult;
  auto R = to_tonlib_api(res.stack);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }
  auto res_stack = R.move_as_ok();

  if (res.missing_library) {
    td::Bits256 hash = res.missing_library.value();
    LOG(DEBUG) << "Requesting missing library: " << hash.to_hex();
    std::vector<td::Bits256> req = {hash};
    client_.send_query(ton::lite_api::liteServer_getLibraries(std::move(req)),
                [self = this, res = std::move(res), res_stack = std::move(res_stack), hash,
                 smc = std::move(smc), args = std::move(args), promise = std::move(promise)]
                (td::Result<ton::lite_api::object_ptr<ton::lite_api::liteServer_libraryResult>> r_libraries) mutable
    {
      if (r_libraries.is_error()) {
        LOG(WARNING) << "cannot obtain missing library: " << r_libraries.move_as_error().to_string();
        promise.set_value(tonlib_api::make_object<tonlib_api::smc_runResult>(res.gas_used, std::move(res_stack), res.code));
        return;
      }
      bool found = false, updated = false;
      auto libraries = r_libraries.move_as_ok();
      for (auto& lr : libraries->result_) {
        auto contents = vm::std_boc_deserialize(lr->data_);
        if (contents.is_ok() && contents.ok().not_null()) {
          if (contents.ok()->get_hash().bits().compare(lr->hash_.cbits(), 256)) {
            LOG(WARNING) << "hash mismatch for library " << lr->hash_.to_hex();
            continue;
          }
          found |= (lr->hash_ == hash);
          updated = true;
          self->libraries.set_ref(lr->hash_, contents.move_as_ok());
          LOG(DEBUG) << "registered library " << lr->hash_.to_hex();
        } else {
          LOG(WARNING) << "failed to deserialize library: " << lr->hash_.to_hex();
        }
      }
      if (updated) {
        self->store_libs_to_disk();
      }
      if (!found) {
        LOG(WARNING) << "cannot obtain library " << hash.to_hex() << ", it may not exist";
        promise.set_value(tonlib_api::make_object<tonlib_api::smc_runResult>(res.gas_used, std::move(res_stack), res.code));
      } else {
        self->perform_smc_execution(std::move(smc), std::move(args), std::move(promise));
      }
    });
  }
  else {
    promise.set_value(tonlib_api::make_object<tonlib_api::smc_runResult>(res.gas_used, std::move(res_stack), res.code));
  }
}

td::Result<tonlib_api::object_ptr<tonlib_api::dns_EntryData>> to_tonlib_api(
    const ton::ManualDns::EntryData& entry_data) {
  td::Result<tonlib_api::object_ptr<tonlib_api::dns_EntryData>> res;
  if (entry_data.data.empty()) {
    return TonlibError::Internal("Unexpected empty EntryData");
  }
  entry_data.data.visit(td::overloaded(
      [&](const ton::ManualDns::EntryDataText& text) {
        res = tonlib_api::make_object<tonlib_api::dns_entryDataText>(text.text);
      },
      [&](const ton::ManualDns::EntryDataNextResolver& resolver) {
        res = tonlib_api::make_object<tonlib_api::dns_entryDataNextResolver>(
            tonlib_api::make_object<tonlib_api::accountAddress>(resolver.resolver.rserialize(true)));
      },
      [&](const ton::ManualDns::EntryDataAdnlAddress& adnl_address) {
        res = tonlib_api::make_object<tonlib_api::dns_entryDataAdnlAddress>(
            tonlib_api::make_object<tonlib_api::adnlAddress>(
                td::adnl_id_encode(adnl_address.adnl_address.as_slice()).move_as_ok()));
      },
      [&](const ton::ManualDns::EntryDataSmcAddress& smc_address) {
        res = tonlib_api::make_object<tonlib_api::dns_entryDataSmcAddress>(
            tonlib_api::make_object<tonlib_api::accountAddress>(smc_address.smc_address.rserialize(true)));
      },
      [&](const ton::ManualDns::EntryDataStorageAddress& storage_address) {
        res = tonlib_api::make_object<tonlib_api::dns_entryDataStorageAddress>(storage_address.bag_id);
      }));
  return res;
}

void TonlibClient::finish_dns_resolve(std::string name, td::Bits256 category, td::int32 ttl,
                                      td::optional<ton::BlockIdExt> block_id, block::StdAddress address,
                                      DnsFinishData dns_finish_data,
                                      td::Promise<object_ptr<tonlib_api::dns_resolved>>&& promise) {
  block_id = dns_finish_data.block_id;
  // TODO: check if the smartcontract supports Dns interface
  // TODO: should we use some DnsInterface instead of ManualDns?
  auto dns = ton::ManualDns::create(dns_finish_data.smc_state, std::move(address));
  TRY_RESULT_PROMISE(promise, entries, dns->resolve(name, category));

  if (entries.size() == 1 && entries[0].partially_resolved && ttl > 0) {
    td::Slice got_name = entries[0].name;
    if (got_name.size() > name.size()) {
      TRY_STATUS_PROMISE(promise, TonlibError::Internal("domain is too long"));
    }
    auto suffix_start = name.size() - got_name.size();
    auto suffix = name.substr(suffix_start);
    if (suffix != got_name) {
      TRY_STATUS_PROMISE(promise, TonlibError::Internal("domain is not a suffix of the query"));
    }
    auto prefix = name.substr(0, suffix_start);
    if (!prefix.empty() && prefix.back() != '.' && suffix[0] != '.') {
      TRY_STATUS_PROMISE(promise, td::Status::Error("next resolver error: domain split not at a component boundary "));
    }

    auto address = entries[0].data.data.get<ton::ManualDns::EntryDataNextResolver>().resolver;
    return do_dns_request(prefix, category, ttl - 1, std::move(block_id), address, std::move(promise));
  }

  std::vector<tonlib_api::object_ptr<tonlib_api::dns_entry>> api_entries;
  for (auto& entry : entries) {
    TRY_RESULT_PROMISE(promise, entry_data, to_tonlib_api(entry.data));
    api_entries.push_back(
        tonlib_api::make_object<tonlib_api::dns_entry>(entry.name, entry.category, std::move(entry_data)));
  }
  promise.set_value(tonlib_api::make_object<tonlib_api::dns_resolved>(std::move(api_entries)));
}

void TonlibClient::do_dns_request(std::string name, td::Bits256 category, td::int32 ttl,
                                  td::optional<ton::BlockIdExt> block_id, block::StdAddress address,
                                  td::Promise<object_ptr<tonlib_api::dns_resolved>>&& promise) {
  auto block_id_copy = block_id.copy();
  td::Promise<DnsFinishData> new_promise =
      promise.send_closure(actor_id(this), &TonlibClient::finish_dns_resolve, name, category, ttl, std::move(block_id),
                           address);

  if (0) {
    make_request(int_api::GetAccountState{address, std::move(block_id_copy), {}},
                 new_promise.wrap([](auto&& account_state) {
                   return DnsFinishData{account_state->get_block_id(), account_state->get_smc_state()};
                 }));

    return;
  }

  TRY_RESULT_PROMISE(promise, args, ton::DnsInterface::resolve_args(name, category, address));
  int_api::RemoteRunSmcMethod query;
  query.address = std::move(address);
  query.args = std::move(args);
  query.block_id = std::move(block_id_copy);
  query.need_result = false;

  make_request(std::move(query), new_promise.wrap([](auto&& run_method) {
    return DnsFinishData{run_method.block_id, run_method.smc_state};
  }));
  ;
}

td::Status TonlibClient::do_request(const tonlib_api::dns_resolve& request,
                                    td::Promise<object_ptr<tonlib_api::dns_resolved>>&& promise) {
  auto block_id = query_context_.block_id.copy();
  if (!request.account_address_) {
    make_request(int_api::GetDnsResolver{},
                 promise.send_closure(actor_id(this), &TonlibClient::do_dns_request, request.name_, request.category_,
                                      request.ttl_, std::move(block_id)));
    return td::Status::OK();
  }
  std::string name = request.name_;
  if (name.empty() || name.back() != '.') {
    name += '.';
  }
  TRY_RESULT(account_address, get_account_address(request.account_address_->account_address_));
  do_dns_request(name, request.category_, request.ttl_, std::move(block_id), account_address,
                 std::move(promise));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(tonlib_api::pchan_signPromise& request,
                                    td::Promise<object_ptr<tonlib_api::pchan_promise>>&& promise) {
  if (!request.promise_) {
    return TonlibError::EmptyField("promise");
  }
  if (!request.input_key_) {
    return TonlibError::EmptyField("input_key");
  }
  TRY_RESULT(input_key, from_tonlib(*request.input_key_));
  make_request(int_api::GetPrivateKey{std::move(input_key)},
               promise.wrap([promise = std::move(request.promise_)](auto key) mutable {
                 auto private_key = td::Ed25519::PrivateKey(std::move(key.private_key));
                 promise->signature_ = ton::pchan::SignedPromiseBuilder()
                                           .promise_A(promise->promise_A_)
                                           .promise_B(promise->promise_B_)
                                           .channel_id(promise->channel_id_)
                                           .with_key(&private_key)
                                           .calc_signature()
                                           .as_slice()
                                           .str();
                 return std::move(promise);
               }));
  return td::Status::OK();
}
td::Status TonlibClient::do_request(tonlib_api::pchan_validatePromise& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  if (!request.promise_) {
    return TonlibError::EmptyField("promise");
  }
  TRY_RESULT(key_bytes, get_public_key(request.public_key_));
  auto key = td::Ed25519::PublicKey(td::SecureString(key_bytes.key));
  bool is_ok = ton::pchan::SignedPromiseBuilder()
                   .promise_A(request.promise_->promise_A_)
                   .promise_B(request.promise_->promise_B_)
                   .channel_id(request.promise_->channel_id_)
                   .check_signature(request.promise_->signature_, key);
  if (!is_ok) {
    return TonlibError::InvalidSignature();
  }
  promise.set_value(tonlib_api::make_object<tonlib_api::ok>());
  return td::Status::OK();
}
td::Status TonlibClient::do_request(tonlib_api::pchan_packPromise& request,
                                    td::Promise<object_ptr<tonlib_api::data>>&& promise) {
  if (!request.promise_) {
    return TonlibError::EmptyField("promise");
  }
  promise.set_value(tonlib_api::make_object<tonlib_api::data>(
      td::SecureString(to_bytes(ton::pchan::SignedPromiseBuilder()
                                    .promise_A(request.promise_->promise_A_)
                                    .promise_B(request.promise_->promise_B_)
                                    .channel_id(request.promise_->channel_id_)
                                    .signature(td::SecureString(request.promise_->signature_))
                                    .finalize()))));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(tonlib_api::pchan_unpackPromise& request,
                                    td::Promise<object_ptr<tonlib_api::pchan_promise>>&& promise) {
  TRY_RESULT_PREFIX(body, vm::std_boc_deserialize(request.data_), TonlibError::InvalidBagOfCells("data"));
  ton::pchan::SignedPromise spromise;
  if (!spromise.unpack(body)) {
    return TonlibError::InvalidField("data", "Can't unpack as a promise");
  }
  promise.set_value(tonlib_api::make_object<tonlib_api::pchan_promise>(
      spromise.o_signature.value().as_slice().str(), spromise.promise.promise_A, spromise.promise.promise_B,
      spromise.promise.channel_id));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(tonlib_api::sync& request,
                                    td::Promise<object_ptr<tonlib_api::ton_blockIdExt>>&& promise) {
  // ton.blockIdExt workchain:int32 shard:int64 seqno:int32 root_hash:bytes file_hash:bytes = ton.BlockIdExt;
  client_.with_last_block(
      std::move(promise).wrap([](auto last_block) -> td::Result<tonlib_api::object_ptr<tonlib_api::ton_blockIdExt>> {
        return to_tonlib_api(last_block.last_block_id);
      }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::createNewKey& request,
                                    td::Promise<object_ptr<tonlib_api::key>>&& promise) {
  TRY_RESULT_PREFIX(
      key,
      key_storage_.create_new_key(std::move(request.local_password_), std::move(request.mnemonic_password_),
                                  std::move(request.random_extra_seed_)),
      TonlibError::Internal());
  TRY_RESULT(key_bytes, public_key_from_bytes(key.public_key.as_slice()));
  promise.set_value(tonlib_api::make_object<tonlib_api::key>(key_bytes.serialize(true), std::move(key.secret)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::exportKey& request,
                                    td::Promise<object_ptr<tonlib_api::exportedKey>>&& promise) {
  if (!request.input_key_) {
    return TonlibError::EmptyField("input_key");
  }
  TRY_RESULT(input_key, from_tonlib(*request.input_key_));
  TRY_RESULT(exported_key, key_storage_.export_key(std::move(input_key)));
  promise.set_value(tonlib_api::make_object<tonlib_api::exportedKey>(std::move(exported_key.mnemonic_words)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::deleteKey& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  if (!request.key_) {
    return TonlibError::EmptyField("key");
  }
  TRY_RESULT(key_bytes, get_public_key(request.key_->public_key_));
  KeyStorage::Key key;
  key.public_key = td::SecureString(key_bytes.key);
  key.secret = std::move(request.key_->secret_);
  TRY_STATUS_PREFIX(key_storage_.delete_key(key), TonlibError::KeyUnknown());
  promise.set_value(tonlib_api::make_object<tonlib_api::ok>());
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::deleteAllKeys& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  TRY_STATUS_PREFIX(key_storage_.delete_all_keys(), TonlibError::Internal());
  promise.set_value(tonlib_api::make_object<tonlib_api::ok>());
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::importKey& request,
                                    td::Promise<object_ptr<tonlib_api::key>>&& promise) {
  if (!request.exported_key_) {
    return TonlibError::EmptyField("exported_key");
  }
  // Note: the mnemonic is considered valid if a certain hash starts with zero byte (see Mnemonic::is_basic_seed())
  // Therefore, importKey with invalid password has 1/256 chance to return OK
  TRY_RESULT(key, key_storage_.import_key(std::move(request.local_password_), std::move(request.mnemonic_password_),
                                          KeyStorage::ExportedKey{std::move(request.exported_key_->word_list_)}));
  TRY_RESULT(key_bytes, public_key_from_bytes(key.public_key.as_slice()));
  promise.set_value(tonlib_api::make_object<tonlib_api::key>(key_bytes.serialize(true), std::move(key.secret)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::exportPemKey& request,
                                    td::Promise<object_ptr<tonlib_api::exportedPemKey>>&& promise) {
  if (!request.input_key_) {
    return TonlibError::EmptyField("input_key");
  }
  TRY_RESULT(input_key, from_tonlib(*request.input_key_));
  TRY_RESULT(exported_pem_key, key_storage_.export_pem_key(std::move(input_key), std::move(request.key_password_)));
  promise.set_value(tonlib_api::make_object<tonlib_api::exportedPemKey>(std::move(exported_pem_key.pem)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::importPemKey& request,
                                    td::Promise<object_ptr<tonlib_api::key>>&& promise) {
  if (!request.exported_key_) {
    return TonlibError::EmptyField("exported_key");
  }
  TRY_RESULT(key, key_storage_.import_pem_key(std::move(request.local_password_), std::move(request.key_password_),
                                              KeyStorage::ExportedPemKey{std::move(request.exported_key_->pem_)}));
  TRY_RESULT(key_bytes, public_key_from_bytes(key.public_key.as_slice()));
  promise.set_value(tonlib_api::make_object<tonlib_api::key>(key_bytes.serialize(true), std::move(key.secret)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::exportEncryptedKey& request,
                                    td::Promise<object_ptr<tonlib_api::exportedEncryptedKey>>&& promise) {
  if (!request.input_key_) {
    return TonlibError::EmptyField("input_key");
  }
  TRY_RESULT(input_key, from_tonlib(*request.input_key_));
  TRY_RESULT(exported_key, key_storage_.export_encrypted_key(std::move(input_key), request.key_password_));
  promise.set_value(tonlib_api::make_object<tonlib_api::exportedEncryptedKey>(std::move(exported_key.data)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::importEncryptedKey& request,
                                    td::Promise<object_ptr<tonlib_api::key>>&& promise) {
  if (!request.exported_encrypted_key_) {
    return TonlibError::EmptyField("exported_encrypted_key");
  }
  TRY_RESULT(key, key_storage_.import_encrypted_key(
                      std::move(request.local_password_), std::move(request.key_password_),
                      KeyStorage::ExportedEncryptedKey{std::move(request.exported_encrypted_key_->data_)}));
  TRY_RESULT(key_bytes, public_key_from_bytes(key.public_key.as_slice()));
  promise.set_value(tonlib_api::make_object<tonlib_api::key>(key_bytes.serialize(true), std::move(key.secret)));
  return td::Status::OK();
}
td::Status TonlibClient::do_request(const tonlib_api::exportUnencryptedKey& request,
                                    td::Promise<object_ptr<tonlib_api::exportedUnencryptedKey>>&& promise) {
  if (!request.input_key_) {
    return TonlibError::EmptyField("input_key");
  }
  TRY_RESULT(input_key, from_tonlib(*request.input_key_));
  TRY_RESULT(exported_key, key_storage_.export_unencrypted_key(std::move(input_key)));
  promise.set_value(tonlib_api::make_object<tonlib_api::exportedUnencryptedKey>(std::move(exported_key.data)));
  return td::Status::OK();
}
td::Status TonlibClient::do_request(const tonlib_api::importUnencryptedKey& request,
                                    td::Promise<object_ptr<tonlib_api::key>>&& promise) {
  if (!request.exported_unencrypted_key_) {
    return TonlibError::EmptyField("exported_encrypted_key");
  }
  TRY_RESULT(key, key_storage_.import_unencrypted_key(
                      std::move(request.local_password_),
                      KeyStorage::ExportedUnencryptedKey{std::move(request.exported_unencrypted_key_->data_)}));
  TRY_RESULT(key_bytes, public_key_from_bytes(key.public_key.as_slice()));
  promise.set_value(tonlib_api::make_object<tonlib_api::key>(key_bytes.serialize(true), std::move(key.secret)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::changeLocalPassword& request,
                                    td::Promise<object_ptr<tonlib_api::key>>&& promise) {
  if (!request.input_key_) {
    return TonlibError::EmptyField("input_key");
  }
  TRY_RESULT(input_key, from_tonlib(*request.input_key_));
  TRY_RESULT(key, key_storage_.change_local_password(std::move(input_key), std::move(request.new_local_password_)));
  promise.set_value(tonlib_api::make_object<tonlib_api::key>(key.public_key.as_slice().str(), std::move(key.secret)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::onLiteServerQueryResult& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  if (ext_client_outbound_.empty()) {
    return TonlibError::InvalidQueryId();
  }
  if (((request.id_ ^ config_generation_) & 0xffff) != 0) {
    return TonlibError::InvalidQueryId();
  }
  send_closure(ext_client_outbound_, &ExtClientOutbound::on_query_result, request.id_ >> 16,
               td::BufferSlice(request.bytes_), to_any_promise(std::move(promise)));
  return td::Status::OK();
}
td::Status TonlibClient::do_request(const tonlib_api::onLiteServerQueryError& request,
                                    td::Promise<object_ptr<tonlib_api::ok>>&& promise) {
  if (ext_client_outbound_.empty()) {
    return TonlibError::InvalidQueryId();
  }
  if (((request.id_ ^ config_generation_) & 0xffff) != 0) {
    return TonlibError::InvalidQueryId();
  }
  send_closure(ext_client_outbound_, &ExtClientOutbound::on_query_result, request.id_ >> 16,
               td::Status::Error(request.error_->code_, request.error_->message_)
                   .move_as_error_prefix(TonlibError::LiteServerNetwork()),
               to_any_promise(std::move(promise)));
  return td::Status::OK();
}

tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(tonlib_api::setLogStream& request) {
  auto result = Logging::set_current_stream(std::move(request.log_stream_));
  if (result.is_ok()) {
    return tonlib_api::make_object<tonlib_api::ok>();
  } else {
    return tonlib_api::make_object<tonlib_api::error>(400, result.message().str());
  }
}
tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(const tonlib_api::getLogStream& request) {
  auto result = Logging::get_current_stream();
  if (result.is_ok()) {
    return result.move_as_ok();
  } else {
    return tonlib_api::make_object<tonlib_api::error>(400, result.error().message().str());
  }
}
tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(
    const tonlib_api::setLogVerbosityLevel& request) {
  auto result = Logging::set_verbosity_level(static_cast<int>(request.new_verbosity_level_));
  if (result.is_ok()) {
    return tonlib_api::make_object<tonlib_api::ok>();
  } else {
    return tonlib_api::make_object<tonlib_api::error>(400, result.message().str());
  }
}
tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(
    const tonlib_api::setLogTagVerbosityLevel& request) {
  auto result = Logging::set_tag_verbosity_level(request.tag_, static_cast<int>(request.new_verbosity_level_));
  if (result.is_ok()) {
    return tonlib_api::make_object<tonlib_api::ok>();
  } else {
    return tonlib_api::make_object<tonlib_api::error>(400, result.message().str());
  }
}
tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(
    const tonlib_api::getLogVerbosityLevel& request) {
  return tonlib_api::make_object<tonlib_api::logVerbosityLevel>(Logging::get_verbosity_level());
}
tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(
    const tonlib_api::getLogTagVerbosityLevel& request) {
  auto result = Logging::get_tag_verbosity_level(request.tag_);
  if (result.is_ok()) {
    return tonlib_api::make_object<tonlib_api::logVerbosityLevel>(result.ok());
  } else {
    return tonlib_api::make_object<tonlib_api::error>(400, result.error().message().str());
  }
}
tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(const tonlib_api::getLogTags& request) {
  return tonlib_api::make_object<tonlib_api::logTags>(Logging::get_tags());
}
tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(const tonlib_api::addLogMessage& request) {
  Logging::add_message(request.verbosity_level_, request.text_);
  return tonlib_api::make_object<tonlib_api::ok>();
}

tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(const tonlib_api::encrypt& request) {
  return tonlib_api::make_object<tonlib_api::data>(
      SimpleEncryption::encrypt_data(request.decrypted_data_, request.secret_));
}

tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(const tonlib_api::decrypt& request) {
  auto r_data = SimpleEncryption::decrypt_data(request.encrypted_data_, request.secret_);
  if (r_data.is_ok()) {
    return tonlib_api::make_object<tonlib_api::data>(r_data.move_as_ok());
  } else {
    return status_to_tonlib_api(r_data.error().move_as_error_prefix(TonlibError::KeyDecrypt()));
  }
}

tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(const tonlib_api::kdf& request) {
  auto max_iterations = 10000000;
  if (request.iterations_ < 0 || request.iterations_ > max_iterations) {
    return status_to_tonlib_api(
        TonlibError::InvalidField("iterations", PSLICE() << "must be between 0 and " << max_iterations));
  }
  return tonlib_api::make_object<tonlib_api::data>(
      SimpleEncryption::kdf(request.password_, request.salt_, request.iterations_));
}

tonlib_api::object_ptr<tonlib_api::Object> TonlibClient::do_static_request(
    const tonlib_api::msg_decryptWithProof& request) {
  if (!request.data_) {
    return status_to_tonlib_api(TonlibError::EmptyField("data"));
  }
  if (!request.data_->data_) {
    TonlibError::EmptyField("data.data");
  }
  if (!request.data_->source_) {
    TonlibError::EmptyField("data.source");
  }
  using ReturnType = tonlib_api::object_ptr<tonlib_api::msg_Data>;
  return downcast_call2<ReturnType>(
      *request.data_->data_,
      td::overloaded([&request](auto&) { return std::move(request.data_->data_); },
                     [&request](tonlib_api::msg_dataEncryptedText& encrypted) -> ReturnType {
                       auto r_decrypted = SimpleEncryptionV2::decrypt_data_with_proof(
                           encrypted.text_, request.proof_, request.data_->source_->account_address_);
                       if (r_decrypted.is_error()) {
                         return std::move(request.data_->data_);
                       }
                       auto decrypted = r_decrypted.move_as_ok();
                       return tonlib_api::make_object<tonlib_api::msg_dataDecryptedText>(decrypted.as_slice().str());
                     }));
}

td::Status TonlibClient::do_request(int_api::GetAccountState request,
                                    td::Promise<td::unique_ptr<AccountState>>&& promise) {
  auto actor_id = actor_id_++;
  actors_[actor_id] = td::actor::create_actor<GetRawAccountState>(
      "GetAccountState", client_.get_client(), request.address, std::move(request.block_id),
      actor_shared(this, actor_id),
      promise.wrap([address = request.address, wallet_id = wallet_id_,
                    o_public_key = std::move(request.public_key)](auto&& state) mutable {
        auto res = td::make_unique<AccountState>(std::move(address), std::move(state), wallet_id);
        if (false && o_public_key) {
          res->guess_type_by_public_key(o_public_key.value());
        }
        return res;
      }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(int_api::GetAccountStateByTransaction request,
                                    td::Promise<td::unique_ptr<AccountState>>&& promise) {
  auto actor_id = actor_id_++;
  actors_[actor_id] = td::actor::create_actor<RunEmulator>(
      "RunEmulator", client_.get_client(), request, actor_shared(this, actor_id),
      promise.wrap([](auto&& state) {
        return std::move(state);
      }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(int_api::RemoteRunSmcMethod request,
                                    td::Promise<int_api::RemoteRunSmcMethod::ReturnType>&& promise) {
  auto actor_id = actor_id_++;
  actors_[actor_id] = td::actor::create_actor<RemoteRunSmcMethod>(
      "RemoteRunSmcMethod", client_.get_client(), std::move(request), actor_shared(this, actor_id), std::move(promise));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(int_api::GetPrivateKey request, td::Promise<KeyStorage::PrivateKey>&& promise) {
  TRY_RESULT(pk, key_storage_.load_private_key(std::move(request.input_key)));
  promise.set_value(std::move(pk));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(int_api::GetDnsResolver request, td::Promise<block::StdAddress>&& promise) {
  client_.with_last_config(promise.wrap([](auto&& state) mutable -> td::Result<block::StdAddress> {
    TRY_RESULT_PREFIX(addr, TRY_VM(state.config->get_dns_root_addr()),
                      TonlibError::Internal("get dns root addr from config: "));
    return block::StdAddress(ton::masterchainId, addr);
  }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(int_api::SendMessage request, td::Promise<td::Unit>&& promise) {
  client_.send_query(ton::lite_api::liteServer_sendMessage(vm::std_boc_serialize(request.message).move_as_ok()),
                     to_any_promise(std::move(promise)));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::liteServer_getInfo& request,
                                    td::Promise<object_ptr<tonlib_api::liteServer_info>>&& promise) {
  client_.send_query(ton::lite_api::liteServer_getVersion(), promise.wrap([](auto&& version) {
    return tonlib_api::make_object<tonlib_api::liteServer_info>(version->now_, version->version_,
                                                                version->capabilities_);
  }));
  return td::Status::OK();
}

auto to_bits256(td::Slice data, td::Slice name) -> td::Result<td::Bits256> {
  if (data.size() != 32) {
    return TonlibError::InvalidField(name, "wrong length (not 32 bytes)");
  }
  return td::Bits256(data.ubegin());
}

td::Status TonlibClient::do_request(tonlib_api::withBlock& request,
                                    td::Promise<object_ptr<tonlib_api::Object>>&& promise) {
  if (!request.id_) {
    return TonlibError::EmptyField("id");
  }
  TRY_RESULT(root_hash, to_bits256(request.id_->root_hash_, "root_hash"));
  TRY_RESULT(file_hash, to_bits256(request.id_->file_hash_, "file_hash"));
  ton::BlockIdExt block_id(request.id_->workchain_, request.id_->shard_, request.id_->seqno_, root_hash, file_hash);
  make_any_request(*request.function_, {std::move(block_id)}, std::move(promise));
  return td::Status::OK();
}

auto to_tonlib_api(const ton::lite_api::tonNode_blockIdExt& blk) -> tonlib_api_ptr<tonlib_api::ton_blockIdExt> {
  return tonlib_api::make_object<tonlib_api::ton_blockIdExt>(
      blk.workchain_, blk.shard_, blk.seqno_, blk.root_hash_.as_slice().str(), blk.file_hash_.as_slice().str());
}

/*auto to_tonlib_api(const ton::BlockIdExt& blk) -> tonlib_api_ptr<tonlib_api::ton_blockIdExt> {
  return tonlib_api::make_object<tonlib_api::ton_blockIdExt>(
      blk.workchain, blk.shard, blk.seqno, blk.root_hash.as_slice().str(), blk.file_hash.as_slice().str());
}*/

auto to_tonlib_api(const ton::lite_api::tonNode_zeroStateIdExt& zeroStateId)
    -> tonlib_api_ptr<tonlib_api::ton_blockIdExt> {
  return tonlib_api::make_object<tonlib_api::ton_blockIdExt>( //TODO check wether shard indeed 0???
      zeroStateId.workchain_, 0, 0, zeroStateId.root_hash_.as_slice().str(), zeroStateId.file_hash_.as_slice().str());
}

auto to_lite_api(const tonlib_api::ton_blockIdExt& blk) -> td::Result<lite_api_ptr<ton::lite_api::tonNode_blockIdExt>> {
  TRY_RESULT(root_hash, to_bits256(blk.root_hash_, "blk.root_hash"))
  TRY_RESULT(file_hash, to_bits256(blk.file_hash_, "blk.file_hash"))
  return ton::lite_api::make_object<ton::lite_api::tonNode_blockIdExt>(
      blk.workchain_, blk.shard_, blk.seqno_, root_hash, file_hash);
}

td::Result<ton::BlockIdExt> to_block_id(const tonlib_api::ton_blockIdExt& blk) {
  TRY_RESULT(root_hash, to_bits256(blk.root_hash_, "blk.root_hash"))
  TRY_RESULT(file_hash, to_bits256(blk.file_hash_, "blk.file_hash"))
  return ton::BlockIdExt(blk.workchain_, blk.shard_, blk.seqno_, root_hash, file_hash);
}

void TonlibClient::get_config_param(int32_t param, int32_t mode, ton::BlockIdExt block, td::Promise<object_ptr<tonlib_api::configInfo>>&& promise) {
  std::vector<int32_t> params = { param };
  client_.send_query(ton::lite_api::liteServer_getConfigParams(mode, ton::create_tl_lite_block_id(block), std::move(params)),
                  promise.wrap([param, block](auto r_config) -> td::Result<object_ptr<tonlib_api::configInfo>> { 
    auto state = block::check_extract_state_proof(block, r_config->state_proof_.as_slice(),
                                                  r_config->config_proof_.as_slice());
    if (state.is_error()) {
      return state.move_as_error_prefix(TonlibError::ValidateConfig());
    }
    auto config = block::Config::extract_from_state(std::move(state.move_as_ok()), 0);
    if (config.is_error()) {
      return config.move_as_error_prefix(TonlibError::ValidateConfig());
    }
    tonlib_api::configInfo config_result;
    config_result.config_ = tonlib_api::make_object<tonlib_api::tvm_cell>(to_bytes(config.move_as_ok()->get_config_param(param)));
    return tonlib_api::make_object<tonlib_api::configInfo>(std::move(config_result));
  }));
}

td::Status TonlibClient::do_request(const tonlib_api::getConfigParam& request,
                        td::Promise<object_ptr<tonlib_api::configInfo>>&& promise) {
  if (query_context_.block_id) {
    get_config_param(request.param_, request.mode_, query_context_.block_id.value(), std::move(promise));
  } else {
    client_.with_last_block([this, promise = std::move(promise), param = request.param_, mode = request.mode_](td::Result<LastBlockState> r_last_block) mutable {       
      if (r_last_block.is_error()) {
        promise.set_error(r_last_block.move_as_error_prefix(TonlibError::Internal("get last block failed ")));
      } else {
        this->get_config_param(param, mode, r_last_block.move_as_ok().last_block_id, std::move(promise));
      }
    });
  }
  return td::Status::OK();
}

void TonlibClient::get_config_all(int32_t mode, ton::BlockIdExt block, td::Promise<object_ptr<tonlib_api::configInfo>>&& promise) {
  client_.send_query(ton::lite_api::liteServer_getConfigAll(mode, ton::create_tl_lite_block_id(block)),
                     promise.wrap([block](auto r_config) -> td::Result<object_ptr<tonlib_api::configInfo>> { 
    auto state = block::check_extract_state_proof(block, r_config->state_proof_.as_slice(),
                                                  r_config->config_proof_.as_slice());
    if (state.is_error()) {
      return state.move_as_error_prefix(TonlibError::ValidateConfig());
    }
    auto config = block::Config::extract_from_state(std::move(state.move_as_ok()), 0);
    if (config.is_error()) {
      return config.move_as_error_prefix(TonlibError::ValidateConfig());
    }
    tonlib_api::configInfo config_result;
    config_result.config_ = tonlib_api::make_object<tonlib_api::tvm_cell>(to_bytes(config.move_as_ok()->get_root_cell()));
    return tonlib_api::make_object<tonlib_api::configInfo>(std::move(config_result));
  }));
}

td::Status TonlibClient::do_request(const tonlib_api::getConfigAll& request,
                        td::Promise<object_ptr<tonlib_api::configInfo>>&& promise) {
  if (query_context_.block_id) {
    get_config_all(request.mode_, query_context_.block_id.value(), std::move(promise));
  } else {
    client_.with_last_block([this, promise = std::move(promise), mode = request.mode_](td::Result<LastBlockState> r_last_block) mutable {       
      if (r_last_block.is_error()) {
        promise.set_error(r_last_block.move_as_error_prefix(TonlibError::Internal("get last block failed ")));
      } else {
        this->get_config_all(mode, r_last_block.move_as_ok().last_block_id, std::move(promise));
      }
    });
  }
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::blocks_getMasterchainInfo& masterchain_info,
                        td::Promise<object_ptr<tonlib_api::blocks_masterchainInfo>>&& promise) {
  client_.send_query(ton::lite_api::liteServer_getMasterchainInfo(),
                     promise.wrap([](lite_api_ptr<ton::lite_api::liteServer_masterchainInfo>&& masterchain_info) {
                       return tonlib_api::make_object<tonlib_api::blocks_masterchainInfo>(
                           to_tonlib_api(*masterchain_info->last_), masterchain_info->state_root_hash_.as_slice().str(),
                           to_tonlib_api(*masterchain_info->init_));
                     }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::blocks_getShards& request,
                                    td::Promise<object_ptr<tonlib_api::blocks_shards>>&& promise) {
  TRY_RESULT(block, to_lite_api(*request.id_))
  TRY_RESULT(req_blk_id, to_block_id(*request.id_));
  client_.send_query(ton::lite_api::liteServer_getAllShardsInfo(std::move(block)),
                     promise.wrap([req_blk_id](lite_api_ptr<ton::lite_api::liteServer_allShardsInfo>&& all_shards_info)
                                      -> td::Result<object_ptr<tonlib_api::blocks_shards>> {
                        auto blk_id = ton::create_block_id(all_shards_info->id_);
                        if (blk_id != req_blk_id) {
                          return td::Status::Error("Liteserver responded with wrong block");
                        }
                        td::BufferSlice proof = std::move((*all_shards_info).proof_);
                        td::BufferSlice data = std::move((*all_shards_info).data_);
                        if (data.empty() || proof.empty()) {
                          return td::Status::Error("Shard configuration or proof is empty");
                        }
                        auto proof_cell = vm::std_boc_deserialize(std::move(proof));
                        if (proof_cell.is_error()) {
                          return proof_cell.move_as_error_prefix("Couldn't deserialize shards proof: ");
                        }
                        auto data_cell = vm::std_boc_deserialize(std::move(data));
                        if (data_cell.is_error()) {
                          return data_cell.move_as_error_prefix("Couldn't deserialize shards data: ");
                        }
                        try {
                          auto virt_root = vm::MerkleProof::virtualize(proof_cell.move_as_ok(), 1);
                          if (virt_root.is_null()) {
                            return td::Status::Error("Virt root is null");
                          }
                          if (ton::RootHash{virt_root->get_hash().bits()} != blk_id.root_hash) {
                            return td::Status::Error("Block shards merkle proof has incorrect root hash");
                          }

                          block::gen::Block::Record blk;
                          block::gen::BlockExtra::Record extra;
                          block::gen::McBlockExtra::Record mc_extra;
                          if (!tlb::unpack_cell(virt_root, blk) || !tlb::unpack_cell(blk.extra, extra) || !extra.custom->have_refs() ||
                              !tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra)) {
                            return td::Status::Error("cannot unpack block extra of block " + blk_id.to_str());
                          }
                          auto data_csr = vm::load_cell_slice_ref(data_cell.move_as_ok());
                          if (data_csr->prefetch_ref()->get_hash() != mc_extra.shard_hashes->prefetch_ref()->get_hash()) {
                            return td::Status::Error("Block shards data and proof hashes don't match");
                          }

                          block::ShardConfig sh_conf;
                          if (!sh_conf.unpack(data_csr)) {
                            return td::Status::Error("cannot extract shard block list from shard configuration");
                          }
                          auto ids = sh_conf.get_shard_hash_ids(true);
                          tonlib_api::blocks_shards shards;
                          for (auto& id : ids) {
                            auto ref = sh_conf.get_shard_hash(ton::ShardIdFull(id));
                            if (ref.not_null()) {
                              shards.shards_.push_back(to_tonlib_api(ref->top_block_id()));
                            }
                          }
                          return tonlib_api::make_object<tonlib_api::blocks_shards>(std::move(shards));
                        } catch (vm::VmError& err) {
                          return err.as_status("Couldn't verify proof: ");
                        } catch (vm::VmVirtError& err) {
                          return err.as_status("Couldn't verify proof: ");
                        } catch (...) {
                          return td::Status::Error("Unknown exception raised while verifying proof");
                        }
                     }));
  return td::Status::OK();
}

td::Status check_lookup_block_proof(lite_api_ptr<ton::lite_api::liteServer_lookupBlockResult>& result, int mode,
                                    ton::BlockId blkid, ton::BlockIdExt client_mc_blkid, td::uint64 lt,
                                    td::uint32 utime);

td::Status TonlibClient::do_request(const tonlib_api::blocks_lookupBlock& request,
                        td::Promise<object_ptr<tonlib_api::ton_blockIdExt>>&& promise) {
  auto lite_block = ton::lite_api::make_object<ton::lite_api::tonNode_blockId>((*request.id_).workchain_, (*request.id_).shard_, (*request.id_).seqno_);
  auto blkid = ton::BlockId(request.id_->workchain_, request.id_->shard_, request.id_->seqno_);
  client_.with_last_block(
    [self = this, blkid, lite_block = std::move(lite_block), mode = request.mode_, lt = (td::uint64)request.lt_, 
    utime = (td::uint32)request.utime_, promise = std::move(promise)](td::Result<LastBlockState> r_last_block) mutable {
      if (r_last_block.is_error()) {
        promise.set_error(r_last_block.move_as_error_prefix(TonlibError::Internal("get last block failed ")));
        return;
      }

      self->client_.send_query(ton::lite_api::liteServer_lookupBlockWithProof(mode, std::move(lite_block), ton::create_tl_lite_block_id(r_last_block.ok().last_block_id), lt, utime),
        promise.wrap([blkid, mode, utime, lt, last_block = r_last_block.ok().last_block_id](lite_api_ptr<ton::lite_api::liteServer_lookupBlockResult>&& result)
                                          -> td::Result<object_ptr<tonlib_api::ton_blockIdExt>> {
          TRY_STATUS(check_lookup_block_proof(result, mode, blkid, last_block, lt, utime));
          return to_tonlib_api(*result->id_);
        })
      );
  });
  return td::Status::OK();
}

td::Status check_lookup_block_proof(lite_api_ptr<ton::lite_api::liteServer_lookupBlockResult>& result, int mode, ton::BlockId blkid, ton::BlockIdExt client_mc_blkid, td::uint64 lt, td::uint32 utime) {
  try {
    ton::BlockIdExt cur_id = ton::create_block_id(result->mc_block_id_);
    if (!cur_id.is_masterchain_ext()) {
      return td::Status::Error("invalid response: mc block id is not from masterchain");
    }
    if (client_mc_blkid != cur_id) {
      auto state = block::check_extract_state_proof(client_mc_blkid, result->client_mc_state_proof_.as_slice(),
                                          result->mc_block_proof_.as_slice());
      if (state.is_error()) {
        LOG(WARNING) << "cannot check state proof: " << state.move_as_error().to_string();
        return state.move_as_error();
      }
      auto state_root = state.move_as_ok();
      auto prev_blocks_dict = block::get_prev_blocks_dict(state_root);
      if (!prev_blocks_dict) {
        return td::Status::Error("cannot extract prev blocks dict from state");
      }

      if (!block::check_old_mc_block_id(*prev_blocks_dict, cur_id)) {
        return td::Status::Error("couldn't check old mc block id");
      }
    }
    try {
      for (auto& link : result->shard_links_) {
        ton::BlockIdExt prev_id = create_block_id(link->id_);
        td::BufferSlice proof = std::move(link->proof_);
        auto R = vm::std_boc_deserialize(proof);
        if (R.is_error()) {
          return TonlibError::InvalidBagOfCells("proof");
        }
        auto block_root = vm::MerkleProof::virtualize(R.move_as_ok(), 1);
        if (cur_id.root_hash != block_root->get_hash().bits()) {
          return td::Status::Error("invalid block hash in proof");
        }
        if (cur_id.is_masterchain()) {
          block::gen::Block::Record blk;
          block::gen::BlockExtra::Record extra;
          block::gen::McBlockExtra::Record mc_extra;
          if (!tlb::unpack_cell(block_root, blk) || !tlb::unpack_cell(blk.extra, extra) || !extra.custom->have_refs() ||
              !tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra)) {
            return td::Status::Error("cannot unpack block header");
          }
          block::ShardConfig shards(mc_extra.shard_hashes->prefetch_ref());
          td::Ref<block::McShardHash> shard_hash = shards.get_shard_hash(prev_id.shard_full(), true);
          if (shard_hash.is_null() || shard_hash->top_block_id() != prev_id) {
            return td::Status::Error("invalid proof chain: prev block is not in mc shard list");
          }
        } else {
          std::vector<ton::BlockIdExt> prev;
          ton::BlockIdExt mc_blkid;
          bool after_split;
          td::Status S = block::unpack_block_prev_blk_try(block_root, cur_id, prev, mc_blkid, after_split);
          if (S.is_error()) {
            return S;
          }
          CHECK(prev.size() == 1 || prev.size() == 2);
          bool found = prev_id == prev[0] || (prev.size() == 2 && prev_id == prev[1]);
          if (!found) {
            return td::Status::Error("invalid proof chain: prev block is not in prev blocks list");
          }
        }
        cur_id = prev_id;
      }
    } catch (vm::VmVirtError& err) {
      return err.as_status();
    }
    if (cur_id.id.workchain != blkid.workchain || !ton::shard_contains(cur_id.id.shard, blkid.shard)) {
      return td::Status::Error("response block has incorrect workchain/shard");
    }

    auto header_r = vm::std_boc_deserialize(std::move(result->header_));
    if (header_r.is_error()) {
      return TonlibError::InvalidBagOfCells("header");
    }
    auto header_root = vm::MerkleProof::virtualize(header_r.move_as_ok(), 1);
    if (header_root.is_null()) {
      return td::Status::Error("header_root is null");
    }
    if (cur_id.root_hash != header_root->get_hash().bits()) {
      return td::Status::Error("invalid header hash in proof");
    }

    std::vector<ton::BlockIdExt> prev;
    ton::BlockIdExt mc_blkid;
    bool after_split;
    auto R = block::unpack_block_prev_blk_try(header_root, cur_id, prev, mc_blkid, after_split);
    if (R.is_error()) {
      return R;
    }
    if (cur_id != ton::create_block_id(result->id_)) {
      return td::Status::Error("response blkid doesn't match header");
    }

    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    if (!(tlb::unpack_cell(header_root, blk) && tlb::unpack_cell(blk.info, info))) {
      return td::Status::Error("block header unpack failed");
    }

    if (mode & 1) {
      if (cur_id.seqno() != blkid.seqno) {
        return td::Status::Error("invalid seqno in proof");
      }
    } else if (mode & 6) {
      auto prev_header_r = vm::std_boc_deserialize(std::move(result->prev_header_));
      if (prev_header_r.is_error()) {
        return TonlibError::InvalidBagOfCells("prev_headers");
      }
      auto prev_header = prev_header_r.move_as_ok();
      auto prev_root = vm::MerkleProof::virtualize(prev_header, 1);
      if (prev_root.is_null()) {
        return td::Status::Error("prev_root is null");
      }

      bool prev_valid = false;
      int prev_idx = -1;
      for (size_t i = 0; i < prev.size(); i++) {
        if (prev[i].root_hash == prev_root->get_hash().bits()) {
          prev_valid = true;
          prev_idx = i;
        }
      }
      if (!prev_valid) {
        return td::Status::Error("invalid prev header hash in proof");
      }
      if (!ton::shard_contains(prev[prev_idx].id.shard, blkid.shard)) {
        return td::Status::Error("invalid prev header shard in proof");
      }

      block::gen::Block::Record prev_blk;
      block::gen::BlockInfo::Record prev_info;
      if (!(tlb::unpack_cell(prev_root, prev_blk) && tlb::unpack_cell(prev_blk.info, prev_info))) {
        return td::Status::Error("prev header unpack failed");
      }

      if (mode & 2) {
        if (prev_info.end_lt > lt) {
          return td::Status::Error("prev header end_lt > lt");
        }
        if (info.end_lt < lt) {
          return td::Status::Error("header end_lt < lt");
        }
      } else if (mode & 4) {
        if (prev_info.gen_utime > utime) {
          return td::Status::Error("prev header end_lt > lt");
        }
        if (info.gen_utime < utime) {
          return td::Status::Error("header end_lt < lt");
        }
      }
    }
  } catch (vm::VmError& err) {
    return td::Status::Error(PSLICE() << "error while checking lookupBlock proof: " << err.get_msg());
  } catch (vm::VmVirtError& err) {
    return td::Status::Error(PSLICE() << "virtualization error while checking lookupBlock proof: " << err.get_msg());
  }

  return td::Status::OK();
}

auto to_tonlib_api(const ton::lite_api::liteServer_transactionId& txid)
    -> tonlib_api_ptr<tonlib_api::blocks_shortTxId> {
  return tonlib_api::make_object<tonlib_api::blocks_shortTxId>(
      txid.mode_, txid.account_.as_slice().str(), txid.lt_, txid.hash_.as_slice().str());
}

td::Status check_block_transactions_proof(lite_api_ptr<ton::lite_api::liteServer_blockTransactions>& bTxes, int32_t mode,
    ton::LogicalTime start_lt, td::Bits256 start_addr, td::Bits256 root_hash, int req_count) {
  if ((mode & ton::lite_api::liteServer_listBlockTransactions::WANT_PROOF_MASK) == 0) {
    return td::Status::OK();
  }
  constexpr int max_answer_transactions = 256;
  bool reverse_mode = mode & ton::lite_api::liteServer_listBlockTransactions::REVERSE_ORDER_MASK;

  try {
    TRY_RESULT(proof_cell, vm::std_boc_deserialize(std::move(bTxes->proof_)));
    auto virt_root = vm::MerkleProof::virtualize(proof_cell, 1);

    if (root_hash != virt_root->get_hash().bits()) {
      return td::Status::Error("Invalid block proof root hash");
    }
    block::gen::Block::Record blk;
    block::gen::BlockExtra::Record extra;
    if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(std::move(blk.extra), extra))) {
      return td::Status::Error("Error unpacking proof cell");
    }
    vm::AugmentedDictionary acc_dict{vm::load_cell_slice_ref(extra.account_blocks), 256,
                block::tlb::aug_ShardAccountBlocks};

    bool eof = false;
    ton::LogicalTime reverse = reverse_mode ? ~0ULL : 0;
    ton::LogicalTime trans_lt = static_cast<ton::LogicalTime>(start_lt);
    td::Bits256 cur_addr = start_addr;
    bool allow_same = true;
    int count = 0;
    while (!eof && count < req_count && count < max_answer_transactions) {
      auto value = acc_dict.extract_value(
            acc_dict.vm::DictionaryFixed::lookup_nearest_key(cur_addr.bits(), 256, !reverse, allow_same));
      if (value.is_null()) {
        eof = true;
        break;
      }
      allow_same = false;
      if (cur_addr != start_addr) {
        trans_lt = reverse;
      }

      block::gen::AccountBlock::Record acc_blk;
      if (!tlb::csr_unpack(std::move(value), acc_blk) || acc_blk.account_addr != cur_addr) {
        return td::Status::Error("Error unpacking proof account block");
      }
      vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                  block::tlb::aug_AccountTransactions};
      td::BitArray<64> cur_trans{(long long)trans_lt};
      while (count < req_count && count < max_answer_transactions) {
        auto tvalue = trans_dict.extract_value_ref(
              trans_dict.vm::DictionaryFixed::lookup_nearest_key(cur_trans.bits(), 64, !reverse));
        if (tvalue.is_null()) {
          trans_lt = reverse;
          break;
        }
        if (static_cast<size_t>(count) < bTxes->ids_.size()) {
          if (mode & 4 && !tvalue->get_hash().bits().equals(bTxes->ids_[count]->hash_.bits(), 256)) {
            return td::Status::Error("Couldn't verify proof (hash)");
          }
          if (mode & 2 && cur_trans != td::BitArray<64>(bTxes->ids_[count]->lt_)) {
            return td::Status::Error("Couldn't verify proof (lt)");
          }
          if (mode & 1 && cur_addr != bTxes->ids_[count]->account_) {
            return td::Status::Error("Couldn't verify proof (account)");
          }
        }
        count++;
      }
    }
    if (static_cast<size_t>(count) != bTxes->ids_.size()) {
      return td::Status::Error(PSLICE() << "Txs count mismatch in proof (" << count << ") and response (" << bTxes->ids_.size() << ")");
    }
  } catch (vm::VmError& err) {
    return err.as_status("Couldn't verify proof: ");
  } catch (vm::VmVirtError& err) {
    return err.as_status("Couldn't verify proof: ");
  } catch (...) {
    return td::Status::Error("Unknown exception raised while verifying proof");
  }
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::blocks_getTransactions& request,
                        td::Promise<object_ptr<tonlib_api::blocks_transactions>>&& promise) {
  TRY_RESULT(block, to_lite_api(*request.id_))
  auto root_hash = block->root_hash_;
  bool check_proof = request.mode_ & ton::lite_api::liteServer_listBlockTransactions::WANT_PROOF_MASK;
  bool reverse_mode = request.mode_ & ton::lite_api::liteServer_listBlockTransactions::REVERSE_ORDER_MASK;
  bool has_starting_tx = request.mode_ & ton::lite_api::liteServer_listBlockTransactions::AFTER_MASK;

  td::Bits256 start_addr;
  ton::LogicalTime start_lt;
  ton::lite_api::object_ptr<ton::lite_api::liteServer_transactionId3> after;
  if (has_starting_tx) {
    if (!request.after_) {
      return td::Status::Error("Missing field `after`");
    }
    TRY_RESULT_ASSIGN(start_addr, to_bits256(request.after_->account_, "account"));
    start_lt = request.after_->lt_;
    after = ton::lite_api::make_object<ton::lite_api::liteServer_transactionId3>(start_addr, start_lt);
  } else {
    start_addr = reverse_mode ? td::Bits256::ones() : td::Bits256::zero();
    start_lt = reverse_mode ? ~0ULL : 0;
    after = nullptr;
  }

  client_.send_query(ton::lite_api::liteServer_listBlockTransactions(
                       std::move(block),
                       request.mode_,
                       request.count_,
                       std::move(after),
                       reverse_mode,
                       check_proof),
                     promise.wrap([root_hash, req_count = request.count_, start_addr, start_lt, mode = request.mode_]
                                  (lite_api_ptr<ton::lite_api::liteServer_blockTransactions>&& bTxes) -> td::Result<object_ptr<tonlib_api::blocks_transactions>> {
                        TRY_STATUS(check_block_transactions_proof(bTxes, mode, start_lt, start_addr, root_hash, req_count));
                        
                        tonlib_api::blocks_transactions r;
                        r.id_ = to_tonlib_api(*bTxes->id_);
                        r.req_count_ = bTxes->req_count_;
                        r.incomplete_ = bTxes->incomplete_;
                        for (auto& id: bTxes->ids_) {
                          r.transactions_.push_back(to_tonlib_api(*id));
                        }
                        return tonlib_api::make_object<tonlib_api::blocks_transactions>(std::move(r));
                     }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::blocks_getTransactionsExt& request,
                        td::Promise<object_ptr<tonlib_api::blocks_transactionsExt>>&& promise) {
  TRY_RESULT(block, to_lite_api(*request.id_))
  bool check_proof = request.mode_ & ton::lite_api::liteServer_listBlockTransactionsExt::WANT_PROOF_MASK;
  bool reverse_mode = request.mode_ & ton::lite_api::liteServer_listBlockTransactionsExt::REVERSE_ORDER_MASK;
  bool has_starting_tx = request.mode_ & ton::lite_api::liteServer_listBlockTransactionsExt::AFTER_MASK;
  
  td::Bits256 start_addr;
  ton::LogicalTime start_lt;
  ton::lite_api::object_ptr<ton::lite_api::liteServer_transactionId3> after;
  if (has_starting_tx) {
    if (!request.after_) {
      return td::Status::Error("Missing field `after`");
    }
    TRY_RESULT_ASSIGN(start_addr, to_bits256(request.after_->account_, "account"));    
    start_lt = request.after_->lt_;
    after = ton::lite_api::make_object<ton::lite_api::liteServer_transactionId3>(start_addr, start_lt);
  } else {
    start_addr = reverse_mode ? td::Bits256::ones() : td::Bits256::zero();
    start_lt = reverse_mode ? ~0ULL : 0;
    after = nullptr;
  }
  auto block_id = ton::create_block_id(block);
  client_.send_query(ton::lite_api::liteServer_listBlockTransactionsExt(
                      std::move(block),
                      request.mode_,
                      request.count_,
                      std::move(after),
                      reverse_mode,
                      check_proof),
                     promise.wrap([block_id, check_proof, reverse_mode, start_addr, start_lt, req_count = request.count_]
                                  (lite_api_ptr<ton::lite_api::liteServer_blockTransactionsExt>&& bTxes) -> td::Result<tonlib_api::object_ptr<tonlib_api::blocks_transactionsExt>> {
                        if (block_id != create_block_id(bTxes->id_)) {
                          return td::Status::Error("Liteserver responded with wrong block");
                        }
                        
                        block::BlockTransactionList list;
                        list.blkid = block_id;
                        list.transactions_boc = std::move(bTxes->transactions_);
                        list.proof_boc = std::move(bTxes->proof_);
                        list.reverse_mode = reverse_mode;
                        list.start_lt = start_lt;
                        list.start_addr = start_addr;
                        list.req_count = req_count;
                        auto info = list.validate(check_proof);
                        if (info.is_error()) {
                          return info.move_as_error_prefix("Validation of block::BlockTransactionList failed: ");
                        }

                        auto raw_transactions = ToRawTransactions(td::optional<td::Ed25519::PrivateKey>()).to_raw_transactions(info.move_as_ok());
                        if (raw_transactions.is_error()) {
                          return raw_transactions.move_as_error_prefix("Error occured while creating tonlib_api::raw_transaction: ");
                        }

                        tonlib_api::blocks_transactionsExt r;
                        r.id_ = to_tonlib_api(*bTxes->id_);
                        r.req_count_ = bTxes->req_count_;
                        r.incomplete_ = bTxes->incomplete_;
                        r.transactions_ = raw_transactions.move_as_ok();
                        return tonlib_api::make_object<tonlib_api::blocks_transactionsExt>(std::move(r));
                     }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::blocks_getBlockHeader& request,
                        td::Promise<object_ptr<tonlib_api::blocks_header>>&& promise) {
  TRY_RESULT(lite_block, to_lite_api(*request.id_))
  TRY_RESULT(req_blk_id, to_block_id(*request.id_));
  client_.send_query(ton::lite_api::liteServer_getBlockHeader(
                       std::move(lite_block),
                       0xffff),
                     promise.wrap([req_blk_id](lite_api_ptr<ton::lite_api::liteServer_blockHeader>&& hdr) -> td::Result<tonlib_api::object_ptr<tonlib_api::blocks_header>> {
                       auto blk_id = ton::create_block_id(hdr->id_);
                       if (blk_id != req_blk_id) {
                         return td::Status::Error("Liteserver responded with wrong block");
                       }
                       auto R = vm::std_boc_deserialize(std::move(hdr->header_proof_));
                       if (R.is_error()) {
                         return R.move_as_error_prefix("Couldn't deserialize header proof: ");
                       } else {
                         auto root = R.move_as_ok();
                         try {
                           auto virt_root = vm::MerkleProof::virtualize(root, 1);
                           if (virt_root.is_null()) {
                             return td::Status::Error("Virt root is null");
                           } else {
                             if (ton::RootHash{virt_root->get_hash().bits()} != blk_id.root_hash) {
                               return td::Status::Error("Block header merkle proof has incorrect root hash");
                             }
                             std::vector<ton::BlockIdExt> prev;
                             ton::BlockIdExt mc_blkid;
                             bool after_split;
                             auto res =
                                 block::unpack_block_prev_blk_ext(virt_root, blk_id, prev, mc_blkid, after_split);
                             if (res.is_error()) {
                               return td::Status::Error("Unpack failed");
                             } else {
                               block::gen::Block::Record blk;
                               block::gen::BlockInfo::Record info;
                               if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(blk.info, info))) {
                                 return td::Status::Error("Unpack failed");
                               } else {
                                 tonlib_api::blocks_header header;
                                 header.id_ = to_tonlib_api(blk_id);
                                 header.global_id_ = blk.global_id;
                                 header.version_ = info.version;
                                 header.flags_ = info.flags;
                                 header.after_merge_ = info.after_merge;
                                 header.after_split_ = info.after_split;
                                 header.before_split_ = info.before_split;
                                 header.want_merge_ = info.want_merge;
                                 header.want_split_ = info.want_split;
                                 header.validator_list_hash_short_ = info.gen_validator_list_hash_short;
                                 header.catchain_seqno_ = info.gen_catchain_seqno;
                                 header.min_ref_mc_seqno_ = info.min_ref_mc_seqno;
                                 header.start_lt_ = info.start_lt;
                                 header.end_lt_ = info.end_lt;
                                 header.gen_utime_ = info.gen_utime;
                                 header.is_key_block_ = info.key_block;
                                 header.vert_seqno_ = info.vert_seq_no;
                                 if (!info.not_master) {
                                   header.prev_key_block_seqno_ = info.prev_key_block_seqno;
                                 }
                                 for (auto& id : prev) {
                                   header.prev_blocks_.push_back(to_tonlib_api(id));
                                 }
                                 return tonlib_api::make_object<tonlib_api::blocks_header>(std::move(header));
                               }
                             }
                           }
                         } catch (vm::VmError& err) {
                           return err.as_status(PSLICE() << "error processing header for " << blk_id.to_str() << " :");
                         } catch (vm::VmVirtError& err) {
                           return err.as_status(PSLICE() << "error processing header for " << blk_id.to_str() << " :");
                         } catch (...) {
                           return td::Status::Error("Unhandled exception catched while processing header");
                         }
                       }
                     }));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::blocks_getMasterchainBlockSignatures& request,
                                    td::Promise<object_ptr<tonlib_api::blocks_blockSignatures>>&& promise) {
  auto actor_id = actor_id_++;
  actors_[actor_id] = td::actor::create_actor<GetMasterchainBlockSignatures>(
      "GetMasterchainBlockSignatures", client_.get_client(), request.seqno_, actor_shared(this, actor_id),
      std::move(promise));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::blocks_getShardBlockProof& request,
                                    td::Promise<object_ptr<tonlib_api::blocks_shardBlockProof>>&& promise) {
  TRY_RESULT(id, to_block_id(*request.id_));
  ton::BlockIdExt from;
  if (request.mode_ & 1) {
    TRY_RESULT_ASSIGN(from, to_block_id(*request.from_));
  }
  auto actor_id = actor_id_++;
  actors_[actor_id] = td::actor::create_actor<GetShardBlockProof>("GetShardBlockProof", client_.get_client(), id, from,
                                                                  actor_shared(this, actor_id), std::move(promise));
  return td::Status::OK();
}

td::Status TonlibClient::do_request(const tonlib_api::blocks_getOutMsgQueueSizes& request,
                                    td::Promise<object_ptr<tonlib_api::blocks_outMsgQueueSizes>>&& promise) {
  client_.send_query(ton::lite_api::liteServer_getOutMsgQueueSizes(request.mode_, request.wc_, request.shard_),
                     promise.wrap([](lite_api_ptr<ton::lite_api::liteServer_outMsgQueueSizes>&& queue_sizes) {
    tonlib_api::blocks_outMsgQueueSizes result;
    result.ext_msg_queue_size_limit_ = queue_sizes->ext_msg_queue_size_limit_;
    for (auto &x : queue_sizes->shards_) {
      tonlib_api::blocks_outMsgQueueSize shard;
      shard.id_ = to_tonlib_api(*x->id_);
      shard.size_ = x->size_;
      result.shards_.push_back(tonlib_api::make_object<tonlib_api::blocks_outMsgQueueSize>(std::move(shard)));
    }
    return tonlib_api::make_object<tonlib_api::blocks_outMsgQueueSizes>(std::move(result));
  }));

  return td::Status::OK();
}

void TonlibClient::load_libs_from_disk() {
  LOG(DEBUG) << "loading libraries from disk cache";
  auto r_data = kv_->get("tonlib.libcache");
  if (r_data.is_error()) {
    return;
  }
  auto r_dict = vm::std_boc_deserialize(r_data.move_as_ok(), true);
  if (r_dict.is_error()) {
    return;
  }
  libraries = vm::Dictionary(vm::load_cell_slice(vm::CellBuilder().append_cellslice(vm::load_cell_slice(
                                                                   r_dict.move_as_ok())).finalize()), 256);

  LOG(DEBUG) << "loaded libraries from disk cache";
}

void TonlibClient::store_libs_to_disk() {  // NB: Dictionary.get_root_cell does not compute_root, and it is protected
  kv_->set("tonlib.libcache", vm::std_boc_serialize(vm::CellBuilder().append_cellslice(libraries.get_root())
                                                        .finalize()).move_as_ok().as_slice());

  LOG(DEBUG) << "stored libraries to disk cache";
}

td::Status TonlibClient::do_request(const int_api::ScanAndLoadGlobalLibs& request, td::Promise<vm::Dictionary> promise) {
  if (request.root.is_null()) {
    promise.set_value(vm::Dictionary{256});
    return td::Status::OK();
  }
  std::set<td::Bits256> to_load;
  std::set<vm::Cell::Hash> visited;
  deep_library_search(to_load, visited, libraries, request.root, 24);
  if (to_load.empty()) {
    promise.set_result(libraries);
    return td::Status::OK();
  }
  std::vector<td::Bits256> to_load_list(to_load.begin(), to_load.end());
  LOG(DEBUG) << "Requesting found libraries in account state (" << to_load_list.size() << ")";
  client_.send_query(
      ton::lite_api::liteServer_getLibraries(std::move(to_load_list)),
      [self = this, promise = std::move(promise)](
          td::Result<ton::lite_api::object_ptr<ton::lite_api::liteServer_libraryResult>> r_libraries) mutable {
        self->process_new_libraries(std::move(r_libraries));
        promise.set_result(self->libraries);
      });
  return td::Status::OK();
}

template <class P>
td::Status TonlibClient::do_request(const tonlib_api::runTests& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::getAccountAddress& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::packAccountAddress& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::unpackAccountAddress& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(tonlib_api::getBip39Hints& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(tonlib_api::setLogStream& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::getLogStream& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::setLogVerbosityLevel& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::setLogTagVerbosityLevel& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::getLogVerbosityLevel& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::getLogTagVerbosityLevel& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::getLogTags& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::addLogMessage& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::encrypt& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::decrypt& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::kdf& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
template <class P>
td::Status TonlibClient::do_request(const tonlib_api::msg_decryptWithProof& request, P&&) {
  UNREACHABLE();
  return TonlibError::Internal();
}
}  // namespace tonlib
