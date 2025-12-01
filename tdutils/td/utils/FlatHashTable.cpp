#include "td/utils/FlatHashTable.h"
#include "td/utils/Random.h"
#include "td/utils/bits.h"

namespace td {
namespace detail {

uint32 normalize_flat_hash_table_size(uint32 size) {
  return td::max(static_cast<uint32>(1) << (32 - count_leading_zeroes32(size)), static_cast<uint32>(8));
}

uint32 get_random_flat_hash_table_bucket(uint32 bucket_count_mask) {
  return Random::fast_uint32() & bucket_count_mask;
}

}  // namespace detail
}  // namespace td
