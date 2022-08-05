#include "crypto/common/refcnt.hpp"
#include "ton/ton-types.h"
#include "crypto/vm/cells.h"
#include "block/transaction.h"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/mc-config.h"
#include "validator/impl/collator.h"

using td::Ref;

class TransactionEmulator {
  public:
    td::Result<td::Ref<vm::Cell>> emulate_transaction(td::Ref<vm::Cell> account_root, td::Ref<vm::Cell> msg_root, td::Ref<vm::Cell> c7_root) {
        // create block::ConfigInfo from c7_root
        auto c7_slice = vm::load_cell_slice(c7_root);
        block::gen::SmartContractInfo::Record smc_info;
        if (!tlb::unpack(c7_slice, smc_info)) {
            return td::Status::Error("cannot unpack c7");
        }
        auto global_config_root = vm::CellBuilder().append_cellslice(*smc_info.global_config).finalize();
        auto res = block::ConfigInfo::extract_config(global_config_root);
        if (res.is_error()) {
            return td::Status::Error("cannot extract config from c7");
        }
        auto global_config = res.move_as_ok();

        // create block::Account from account_root
        auto account_slice = vm::load_cell_slice(account_root);
        block::gen::Account::Record_account account;
        if (!tlb::unpack(account_slice, account)) {
            return td::Status::Error("cannot unpack account");
        }

        ton::WorkchainId wc;
        ton::StdSmcAddress addr;
        if (!block::tlb::t_MsgAddressInt.extract_std_address(account.addr, wc, addr)) {
            return td::Status::Error("cannot extract account address");
        }

        td::Bits256 last_trans_hash = {};
        td::uint64 last_trans_lt = 0;
        auto shard_account_root = vm::CellBuilder().store_ref(account_root).store_bits(last_trans_hash.as_bitslice()).store_long(last_trans_lt).finalize();

        auto account_ptr = std::make_unique<block::Account>(wc, addr.bits());
        if (!account_ptr->unpack(vm::load_cell_slice_ref(shard_account_root), td::Ref<vm::CellSlice>(), global_config->utime,
                            wc == ton::masterchainId && global_config->is_special_smartcontract(addr))) {
            return td::Status::Error("cannot unpack shard account");;
        }

        return emulate_transaction(std::move(account_ptr), std::move(msg_root), std::move(global_config));
    }

    td::Result<td::Ref<vm::Cell>> emulate_transaction(std::unique_ptr<block::Account> account, td::Ref<vm::Cell> msg_root, 
                                                      std::unique_ptr<block::ConfigInfo> config) {
        auto cs = vm::load_cell_slice(msg_root);
        bool external;
        Ref<vm::CellSlice> src, dest;
        switch (block::gen::t_CommonMsgInfo.get_tag(cs)) {
            case block::gen::CommonMsgInfo::ext_in_msg_info: {
                block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
                if (!tlb::unpack(cs, info)) {
                    return td::Status::Error("cannot unpack inbound external message");
                }
                dest = std::move(info.dest);
                external = true;
                break;
            }
            case block::gen::CommonMsgInfo::int_msg_info: {
                block::gen::CommonMsgInfo::Record_int_msg_info info;
                if (!tlb::unpack(cs, info)) {
                    return td::Status::Error("cannot unpack internal message to be processed by an ordinary transaction");
                }
                src = std::move(info.src);
                dest = std::move(info.dest);
                external = false;
                break;
            }
            default:
                return td::Status::Error("Only ext in and int message are supported");
        }

        ton::WorkchainId wc;
        ton::StdSmcAddress addr;
        if (!block::tlb::t_MsgAddressInt.extract_std_address(dest, wc, addr)) {
            return td::Status::Error("cannot extract message address");
        }

        td::Ref<vm::Cell> old_mparams;
        std::vector<block::StoragePrices> storage_prices_;
        block::StoragePhaseConfig storage_phase_cfg_{&storage_prices_};
        td::BitArray<256> rand_seed_;
        block::ComputePhaseConfig compute_phase_cfg_;
        block::ActionPhaseConfig action_phase_cfg_;
        td::RefInt256 masterchain_create_fee, basechain_create_fee;
        
        auto fetch_res = ton::Collator::impl_fetch_config_params(std::move(config), &old_mparams,
                                            &storage_prices_, &storage_phase_cfg_,
                                            &rand_seed_, &compute_phase_cfg_,
                                            &action_phase_cfg_, &masterchain_create_fee,
                                            &basechain_create_fee, wc);
        if(fetch_res.is_error()) {
            return fetch_res.move_as_error_prefix("Cannot fetch config params ");
        }

        auto utime = config->utime;
        auto lt = config->lt;

        auto res = ton::Collator::impl_create_ordinary_transaction(msg_root, account.get(), utime, lt,
                                                        &storage_phase_cfg_, &compute_phase_cfg_,
                                                        &action_phase_cfg_,
                                                        external, lt);
        if(res.is_error()) {
            return res.move_as_error_prefix("Cannot run message on account ");
        }
        std::unique_ptr<block::Transaction> trans = res.move_as_ok();

        auto trans_root = trans->commit(*account);
        if (trans_root.is_null()) {
            return td::Status::Error(PSLICE() << "Cannot commit new transaction for smart contract");
        }
        return trans_root;
    }
};