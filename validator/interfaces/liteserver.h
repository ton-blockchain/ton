#pragma once

#include "td/actor/actor.h"

namespace ton {

namespace validator {

class LiteServerCache : public td::actor::Actor {
 public:
  virtual ~LiteServerCache() = default;
};

}  // namespace validator

}  // namespace ton
