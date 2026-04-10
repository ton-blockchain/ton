/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "storage-stat-cache.hpp"

namespace ton::validator {

void StorageStatCache::get_cache(td::Promise<std::function<td::Ref<vm::Cell>(const td::Bits256&)>> promise) {
  LOG(DEBUG) << "StorageStatCache::get_cache";
  promise.set_value(
      [cache = cache_](const td::Bits256& hash) mutable -> td::Ref<vm::Cell> { return cache.lookup_ref(hash); });
}

void StorageStatCache::update(std::vector<std::pair<td::Ref<vm::Cell>, td::uint32>> data) {
  for (auto& [cell, size] : data) {
    if (size < MIN_ACCOUNT_CELLS) {
      continue;
    }
    td::Bits256 hash = cell->get_hash().bits();
    LOG(DEBUG) << "StorageStatCache::update " << hash.to_hex() << " " << size;
    cache_.set_ref(hash, cell);
    lru_.put(hash, Deleter{hash, &cache_}, true, size);
  }
}

}  // namespace ton::validator
