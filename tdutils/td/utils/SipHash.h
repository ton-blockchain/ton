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
#pragma once

#include "td/utils/Slice.h"
#include "td/utils/common.h"

namespace td {

// SipHash-1-3. Unlike the hashes in Hash.h and HashTableUtils.h this one is keyed, so an attacker
// cannot construct keys that land in the same bucket without knowing the key. Use it for hash tables
// whose keys can be chosen or influenced by a peer; the unkeyed hashes are fine for everything else
// and are cheaper.
uint64 sip_hash13(Slice data, uint64 k0, uint64 k1);

// Keyed with a value drawn once per process from the secure RNG, so bucket collisions cannot be
// precomputed offline. Values are not stable across runs and must not be persisted or sent.
uint64 sip_hash13(Slice data);

}  // namespace td
