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

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "persistent-vector.h"

#include "auto/tl/ton_api.h"

#include "adnl/utils.hpp"

namespace ton {

namespace validatorsession {

HashType get_vector_hash(ValidatorSessionDescription& desc, std::vector<HashType>&& value) {
  std::vector<td::int32> v;
  v.resize(value.size());
  for (size_t i = 0; i < v.size(); i++) {
    v[i] = value[i];
  }
  auto obj = ton::create_tl_object<ton::ton_api::hashable_vector>(std::move(v));
  return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
}

HashType get_vs_hash(ValidatorSessionDescription& desc, const td::uint32& value) {
  auto obj = ton::create_tl_object<ton::ton_api::hashable_int32>(value);
  return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
}
HashType get_vs_hash(ValidatorSessionDescription& desc, const td::Bits256& value) {
  auto obj = ton::create_tl_object<ton::ton_api::hashable_int256>(value);
  return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
}
HashType get_vs_hash(ValidatorSessionDescription& desc, const td::uint64& value) {
  auto obj = ton::create_tl_object<ton::ton_api::hashable_int64>(value);
  return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
}
HashType get_vs_hash(ValidatorSessionDescription& desc, const bool& value) {
  auto obj = ton::create_tl_object<ton::ton_api::hashable_bool>(value);
  return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
}
HashType get_vs_hash(ValidatorSessionDescription& desc, const td::BufferSlice& value) {
  auto obj = ton::create_tl_object<ton::ton_api::hashable_bytes>(value.clone());
  return desc.compute_hash(serialize_tl_object(obj, true).as_slice());
}

}  // namespace validatorsession

}  // namespace ton
