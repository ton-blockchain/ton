#pragma once

//#include "td/utils/FlatHashMapChunks.h"
#include <functional>

#include "td/utils/FlatHashTable.h"
#include "td/utils/HashTableUtils.h"
#include "td/utils/SetNode.h"
//#include <unordered_set>

namespace td {

template <class KeyT, class HashT = Hash<KeyT>, class EqT = std::equal_to<KeyT>>
using FlatHashSet = FlatHashTable<SetNode<KeyT, EqT>, HashT, EqT>;
//using FlatHashSet = FlatHashSetChunks<KeyT, HashT, EqT>;
//using FlatHashSet = std::unordered_set<KeyT, HashT, EqT>;

}  // namespace td
