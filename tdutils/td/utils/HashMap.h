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
#pragma once

#include "td/utils/Hash.h"

#if TD_HAVE_ABSL
#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>
#include <absl/container/btree_map.h>
#else
#include <unordered_map>
#include <map>
#endif

namespace td {

#if TD_HAVE_ABSL
template <class Key, class Value, class H = Hash<Key>>
using HashMap = absl::flat_hash_map<Key, Value, H>;
template <class Key, class Value, class H = Hash<Key>, class E = std::equal_to<>>
using NodeHashMap = absl::node_hash_map<Key, Value, H, E>;
template <class Key, class Value>
using BTreeMap = absl::btree_map<Key, Value>;
#else
template <class Key, class Value, class H = Hash<Key>>
using HashMap = std::unordered_map<Key, Value, H>;
template <class Key, class Value, class H = Hash<Key>>
using NodeHashMap = std::unordered_map<Key, Value, H>;
template <class Key, class Value>
using BTreeMap = std::map<Key, Value>;
#endif

}  // namespace td
