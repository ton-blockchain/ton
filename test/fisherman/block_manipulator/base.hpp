#pragma once

#include "crypto/block/block-auto.h"

namespace test::fisherman {

class BaseManipulator {
 public:
  virtual void modify(block::gen::Block::Record &block) = 0;
  virtual ~BaseManipulator() = default;
};

}  // namespace test::fisherman
