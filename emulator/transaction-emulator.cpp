#include "transaction-emulator.h"
#include "crypto/common/refcnt.hpp"
#include "validator/impl/collator.h"

using td::Ref;

td::Result<td::Ref<vm::Cell>> TransactionEmulator::emulate_transaction(td::Ref<vm::Cell> msg_root) {
    if (!config_) {
        return td::Status::Error("config not set");
    }

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
            return td::Status::Error("only ext in and int message are supported");
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
    
    auto fetch_res = ton::Collator::impl_fetch_config_params(std::move(config_), &old_mparams,
                                        &storage_prices_, &storage_phase_cfg_,
                                        &rand_seed_, &compute_phase_cfg_,
                                        &action_phase_cfg_, &masterchain_create_fee,
                                        &basechain_create_fee, wc);
    if(fetch_res.is_error()) {
        return fetch_res.move_as_error_prefix("cannot fetch config params ");
    }
    config_ = fetch_res.move_as_ok();

    compute_phase_cfg_.ignore_chksig = external;

    auto res = ton::Collator::impl_create_ordinary_transaction(msg_root, &account_, config_->utime, config_->lt,
                                                    &storage_phase_cfg_, &compute_phase_cfg_,
                                                    &action_phase_cfg_,
                                                    external, config_->lt);
    if(res.is_error()) {
        return res.move_as_error_prefix("cannot run message on account ");
    }
    std::unique_ptr<block::Transaction> trans = res.move_as_ok();

    auto trans_root = trans->commit(account_);
    if (trans_root.is_null()) {
        return td::Status::Error(PSLICE() << "cannot commit new transaction for smart contract");
    }
    return trans_root;
}
