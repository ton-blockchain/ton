#include "transaction_corrupter.hpp"

#include "block-parse.h"

namespace test::fisherman {

auto TransactionCorrupter::Config::fromJson(td::JsonValue jv) -> Config {
  Config cfg;
  CHECK(jv.type() == td::JsonValue::Type::Object);
  auto& obj = jv.get_object();
  cfg.transaction_fee_change = td::get_json_object_long_field(obj, "transaction_fee_change", false).move_as_ok();
  return cfg;
}

TransactionCorrupter::TransactionCorrupter(Config config) : config_(std::move(config)) {
}

void TransactionCorrupter::modify(block::gen::Block::Record& block) {
  block::gen::BlockExtra::Record block_extra_rec;
  CHECK(block::gen::BlockExtra().cell_unpack(block.extra, block_extra_rec));

  block::gen::ShardAccountBlocks::Record shard_account_blocks;
  CHECK(block::gen::ShardAccountBlocks().cell_unpack(block_extra_rec.account_blocks, shard_account_blocks));

  vm::AugmentedDictionary accounts_dict{shard_account_blocks.x, 256, block::tlb::aug_ShardAccountBlocks};
  vm::AugmentedDictionary new_accounts_dict{256, block::tlb::aug_ShardAccountBlocks};

  accounts_dict.check_for_each_extra(
      [&](td::Ref<vm::CellSlice> account_slice, auto const&, td::ConstBitPtr account_key, int account_key_len) -> bool {
        block::gen::AccountBlock::Record account_block_rec;
        CHECK(block::gen::AccountBlock().unpack(account_slice.write(), account_block_rec));
        vm::AugmentedDictionary tx_dict{vm::DictNonEmpty(), account_block_rec.transactions, 64,
                                        block::tlb::aug_AccountTransactions};
        vm::AugmentedDictionary new_tx_dict{64, block::tlb::aug_AccountTransactions};

        CHECK(tx_dict.check_for_each_extra(
            [&](td::Ref<vm::CellSlice> tx_slice, auto const&, td::ConstBitPtr tx_key, int tx_key_len) -> bool {
              block::gen::Transaction::Record tx_rec;
              if (block::gen::Transaction().cell_unpack(tx_slice.write().fetch_ref(), tx_rec)) {
                block::CurrencyCollection tx_currency_rec;
                CHECK(tx_currency_rec.validate_unpack(tx_rec.total_fees));
                tx_currency_rec.grams += config_.transaction_fee_change;
                tx_currency_rec.pack_to(tx_rec.total_fees);

                td::Ref<vm::Cell> new_tx_cell;
                CHECK(block::gen::Transaction().cell_pack(new_tx_cell, tx_rec));
                new_tx_dict.set_ref(tx_key, tx_key_len, new_tx_cell, vm::Dictionary::SetMode::Add);
              }
              return true;
            },
            false));

        account_block_rec.transactions.write() = vm::load_cell_slice(new_tx_dict.get_root_cell());

        vm::CellBuilder cb2;
        CHECK(block::gen::AccountBlock().pack(cb2, account_block_rec));
        new_accounts_dict.set(account_key, account_key_len, cb2.finalize(), vm::Dictionary::SetMode::Add);

        return true;
      },
      false);
  shard_account_blocks.x = new_accounts_dict.get_root();
  CHECK(block::gen::ShardAccountBlocks().cell_pack(block_extra_rec.account_blocks, shard_account_blocks));
  CHECK(block::gen::BlockExtra().cell_pack(block.extra, block_extra_rec));
}

}  // namespace test::fisherman
