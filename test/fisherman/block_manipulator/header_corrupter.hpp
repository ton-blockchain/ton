#pragma once

#include "base.hpp"
#include "td/utils/JsonBuilder.h"

namespace test::fisherman {

class HeaderCorrupter : public BaseManipulator {
 public:
  struct Config {
    // TODO: add corruption field and method

    static auto fromJson(td::JsonValue jv) -> Config;
  };

  explicit HeaderCorrupter(Config config);
  void modify(block::gen::Block::Record &block) final;

 private:
  Config config_;
};

}  // namespace test::fisherman
