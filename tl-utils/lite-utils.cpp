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
#include "tl-utils.hpp"
#include "tl/tl_object_store.h"
#include "auto/tl/lite_api.hpp"
#include "td/utils/tl_storers.h"
#include "td/utils/crypto.h"
#include "crypto/common/bitstring.h"
#include <map>

namespace ton {

td::BufferSlice serialize_tl_object(const lite_api::Object *T, bool boxed, td::BufferSlice &&suffix) {
  td::TlStorerCalcLength X;
  T->store(X);
  auto l = X.get_length() + (boxed ? 4 : 0);
  auto len = l + suffix.size();

  td::BufferSlice B(len);
  td::TlStorerUnsafe Y(B.as_slice().ubegin());
  if (boxed) {
    Y.store_binary(T->get_id());
  }
  T->store(Y);

  auto S = B.as_slice();
  S.remove_prefix(l);
  S.copy_from(suffix.as_slice());
  suffix.clear();

  return B;
}

td::BufferSlice serialize_tl_object(const lite_api::Object *T, bool boxed) {
  td::TlStorerCalcLength X;
  T->store(X);
  auto l = X.get_length() + (boxed ? 4 : 0);
  auto len = l;

  td::BufferSlice B(len);
  td::TlStorerUnsafe Y(B.as_slice().ubegin());
  if (boxed) {
    Y.store_binary(T->get_id());
  }
  T->store(Y);

  return B;
}

td::BufferSlice serialize_tl_object(const lite_api::Function *T, bool boxed) {
  CHECK(boxed);
  td::TlStorerCalcLength X;
  T->store(X);
  auto l = X.get_length();
  auto len = l;

  td::BufferSlice B(len);
  td::TlStorerUnsafe Y(B.as_slice().ubegin());

  T->store(Y);

  return B;
}

td::BufferSlice serialize_tl_object(const lite_api::Function *T, bool boxed, td::BufferSlice &&suffix) {
  CHECK(boxed);
  td::TlStorerCalcLength X;
  T->store(X);
  auto l = X.get_length();
  auto len = l + suffix.size();

  td::BufferSlice B(len);
  td::TlStorerUnsafe Y(B.as_slice().ubegin());

  T->store(Y);

  auto S = B.as_slice();
  S.remove_prefix(l);
  S.copy_from(suffix.as_slice());
  suffix.clear();

  return B;
}

td::UInt256 get_tl_object_sha256(const lite_api::Object *T) {
  td::TlStorerCalcLength X;
  T->store(X);
  auto len = X.get_length() + 4;

  td::BufferSlice B(len);
  td::TlStorerUnsafe Y(B.as_slice().ubegin());
  Y.store_binary(T->get_id());
  T->store(Y);

  td::UInt256 id256;
  td::sha256(B.as_slice(), id256.as_slice());

  return id256;
}

td::Bits256 get_tl_object_sha_bits256(const lite_api::Object *T) {
  td::TlStorerCalcLength X;
  T->store(X);
  auto len = X.get_length() + 4;

  td::BufferSlice B(len);
  td::TlStorerUnsafe Y(B.as_slice().ubegin());
  Y.store_binary(T->get_id());
  T->store(Y);

  td::Bits256 id256;
  td::sha256(B.as_slice(), id256.as_slice());

  return id256;
}

std::string lite_query_name_by_id(int id) {
  static std::map<int, std::string> names = {
      {lite_api::liteServer_getMasterchainInfo::ID, "getMasterchainInfo"},
      {lite_api::liteServer_getMasterchainInfoExt::ID, "getMasterchainInfoExt"},
      {lite_api::liteServer_getTime::ID, "getTime"},
      {lite_api::liteServer_getVersion::ID, "getVersion"},
      {lite_api::liteServer_getBlock::ID, "getBlock"},
      {lite_api::liteServer_getState::ID, "getState"},
      {lite_api::liteServer_getBlockHeader::ID, "getBlockHeader"},
      {lite_api::liteServer_sendMessage::ID, "sendMessage"},
      {lite_api::liteServer_getAccountState::ID, "getAccountState"},
      {lite_api::liteServer_getAccountStatePrunned::ID, "getAccountStatePrunned"},
      {lite_api::liteServer_runSmcMethod::ID, "runSmcMethod"},
      {lite_api::liteServer_getShardInfo::ID, "getShardInfo"},
      {lite_api::liteServer_getAllShardsInfo::ID, "getAllShardsInfo"},
      {lite_api::liteServer_getOneTransaction::ID, "getOneTransaction"},
      {lite_api::liteServer_getTransactions::ID, "getTransactions"},
      {lite_api::liteServer_lookupBlock::ID, "lookupBlock"},
      {lite_api::liteServer_lookupBlockWithProof::ID, "lookupBlockWithProof"},
      {lite_api::liteServer_listBlockTransactions::ID, "listBlockTransactions"},
      {lite_api::liteServer_listBlockTransactionsExt::ID, "listBlockTransactionsExt"},
      {lite_api::liteServer_getBlockProof::ID, "getBlockProof"},
      {lite_api::liteServer_getConfigAll::ID, "getConfigAll"},
      {lite_api::liteServer_getConfigParams::ID, "getConfigParams"},
      {lite_api::liteServer_getValidatorStats::ID, "getValidatorStats"},
      {lite_api::liteServer_getLibraries::ID, "getLibraries"},
      {lite_api::liteServer_getLibrariesWithProof::ID, "getLibrariesWithProof"},
      {lite_api::liteServer_getShardBlockProof::ID, "getShardBlockProof"},
      {lite_api::liteServer_getOutMsgQueueSizes::ID, "getOutMsgQueueSizes"},
      {lite_api::liteServer_getBlockOutMsgQueueSize::ID, "getBlockOutMsgQueueSize"},
      {lite_api::liteServer_getDispatchQueueInfo::ID, "getDispatchQueueInfo"},
      {lite_api::liteServer_getDispatchQueueMessages::ID, "getDispatchQueueMessages"},
      {lite_api::liteServer_nonfinal_getCandidate::ID, "nonfinal.getCandidate"},
      {lite_api::liteServer_nonfinal_getValidatorGroups::ID, "nonfinal.getValidatorGroups"}};
  auto it = names.find(id);
  if (it == names.end()) {
    return "unknown";
  }
  return it->second;
}

}  // namespace ton
