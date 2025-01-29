#include "header_corrupter.hpp"

namespace test::fisherman {

auto HeaderCorrupter::Config::fromJson(td::JsonValue jv) -> Config {
  return Config{};
}

HeaderCorrupter::HeaderCorrupter(Config config) : config_(std::move(config)) {
}

void HeaderCorrupter::modify(block::gen::Block::Record &block) {
  block::gen::BlockInfo::Record info_rec;
  bool ok = block::gen::BlockInfo().cell_unpack(block.info, info_rec);
  CHECK(ok);
  info_rec.after_merge = true;
  info_rec.after_split = true;
  block::gen::BlockInfo().cell_pack(block.info, info_rec);
}

}  // namespace test::fisherman
