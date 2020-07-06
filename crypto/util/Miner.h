#pragma once

#include "block/block.h"
#include "td/utils/optional.h"
#include "td/utils/Time.h"
#include <atomic>
#include <array>

namespace ton {
class Miner {
 public:
  struct Options {
    block::StdAddress my_address;
    std::array<td::uint8, 16> seed;
    std::array<td::uint8, 32> complexity;
    td::optional<td::Timestamp> expire_at;
    td::int64 max_iterations = std::numeric_limits<td::int64>::max();
    std::atomic<td::uint64>* hashes_computed{nullptr};
  };

  static td::optional<std::string> run(const Options& options);
};
}  // namespace ton
