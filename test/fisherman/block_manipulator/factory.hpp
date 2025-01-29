#pragma once

#include "base.hpp"
#include "td/utils/JsonBuilder.h"

namespace test::fisherman {

class ManipulatorFactory {
 public:
  auto create(td::JsonValue jv) -> std::shared_ptr<BaseManipulator>;

 private:
  auto createImpl(td::JsonValue jv) -> td::Result<std::shared_ptr<BaseManipulator>>;
};

}  // namespace test::fisherman
