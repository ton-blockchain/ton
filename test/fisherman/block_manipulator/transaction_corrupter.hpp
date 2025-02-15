#pragma once

#include "base.hpp"

#include "td/utils/JsonBuilder.h"

namespace test::fisherman {

class TransactionCorrupter : public BaseManipulator {
 public:
  struct Config {
    // TODO: add corruption fields and associate this manipulator with another to achieve more complex block corruptions
    td::int64 transaction_fee_change;

    static auto fromJson(td::JsonValue jv) -> Config;
  };

  explicit TransactionCorrupter(Config config);
  void modify(block::gen::Block::Record &block) final;

 private:
  Config config_;
};

}  // namespace test::fisherman
