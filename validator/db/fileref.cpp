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
#include "fileref.hpp"
#include "auto/tl/ton_api.hpp"
#include "td/utils/overloaded.h"
#include "td/utils/misc.h"

namespace ton {

namespace validator {

namespace {

td::Result<BlockId> get_block_id(std::stringstream& ss) {
  std::string token;
  BlockId block_id;
  std::getline(ss, token, '_');
  TRY_RESULT(w, td::to_integer_safe<WorkchainId>(token));
  std::getline(ss, token, '_');
  TRY_RESULT(shard, td::hex_to_integer_safe<ShardId>(token));
  std::getline(ss, token, '_');
  TRY_RESULT(s, td::to_integer_safe<BlockSeqno>(token));
  return BlockId{w, shard, s};
}

td::Result<FileHash> get_token_hash(std::stringstream& ss) {
  std::string token;
  std::getline(ss, token, '_');
  if (token.size() != 64) {
    return td::Status::Error(ErrorCode::protoviolation, "hash must have exactly 64 hexdigits");
  }

  TRY_RESULT(v, td::hex_decode(token));

  FileHash r;
  r.as_slice().copy_from(v);
  return r;
}

}  // namespace

namespace fileref {

std::string Empty::filename() const {
  return "empty";
}

Empty Empty::shortref() const {
  return *this;
}

std::string Empty::filename_short() const {
  return "empty";
}

BlockShort Block::shortref() const {
  return BlockShort{block_id.id, hash()};
}

std::string Block::filename() const {
  return PSTRING() << "block_" << block_id.to_str();
}

std::string Block::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.id.shard));
  return PSTRING() << "block_" << block_id.id.workchain << "_" << s << "_" << block_id.id.seqno << "_"
                   << hash().to_hex();
}

std::string BlockShort::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.shard));
  return PSTRING() << "block_" << block_id.workchain << "_" << s << "_" << block_id.seqno << "_" << hash().to_hex();
}

ZeroStateShort ZeroState::shortref() const {
  return ZeroStateShort{block_id.id.workchain, hash()};
}

std::string ZeroState::filename() const {
  return PSTRING() << "zerostate_" << block_id.to_str();
}

std::string ZeroState::filename_short() const {
  return PSTRING() << "zerostate_" << block_id.id.workchain << "_" << hash().to_hex();
}

std::string ZeroStateShort::filename_short() const {
  return PSTRING() << "zerostate_" << workchain << "_" << hash().to_hex();
}

PersistentStateShort PersistentState::shortref() const {
  return PersistentStateShort{block_id.shard_full(), masterchain_block_id.seqno(), hash()};
}

std::string PersistentState::filename() const {
  return PSTRING() << "state_" << masterchain_block_id.to_str() << "_" << block_id.to_str();
}

std::string PersistentState::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.id.shard));
  return PSTRING() << "state_" << masterchain_block_id.seqno() << "_" << block_id.id.workchain << "_" << s << "_"
                   << hash().to_hex();
}

std::string PersistentStateShort::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(shard_id.shard));
  return PSTRING() << "state_" << masterchain_seqno << "_" << shard_id.workchain << "_" << s << "_" << hash().to_hex();
}

ProofShort Proof::shortref() const {
  return ProofShort{block_id.id, hash()};
}

std::string Proof::filename() const {
  return PSTRING() << "proof_" << block_id.to_str();
}

std::string Proof::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.id.shard));
  return PSTRING() << "proof_" << block_id.id.workchain << "_" << s << "_" << block_id.id.seqno << "_"
                   << hash().to_hex();
}

std::string ProofShort::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.shard));
  return PSTRING() << "proof_" << block_id.workchain << "_" << s << "_" << block_id.seqno << "_" << hash().to_hex();
}

ProofLinkShort ProofLink::shortref() const {
  return ProofLinkShort{block_id.id, hash()};
}

std::string ProofLink::filename() const {
  return PSTRING() << "prooflink_" << block_id.to_str();
}

std::string ProofLink::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.id.shard));
  return PSTRING() << "prooflink_" << block_id.id.workchain << "_" << s << "_" << block_id.id.seqno << "_"
                   << hash().to_hex();
}

std::string ProofLinkShort::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.shard));
  return PSTRING() << "prooflink_" << block_id.workchain << "_" << s << "_" << block_id.seqno << "_" << hash().to_hex();
}

SignaturesShort Signatures::shortref() const {
  return SignaturesShort{block_id.id, hash()};
}

std::string Signatures::filename() const {
  return PSTRING() << "signatures_" << block_id.to_str();
}

std::string Signatures::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.id.shard));
  return PSTRING() << "signatures_" << block_id.id.workchain << "_" << s << "_" << block_id.id.seqno << "_"
                   << hash().to_hex();
}

std::string SignaturesShort::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.shard));
  return PSTRING() << "signatures_" << block_id.workchain << "_" << s << "_" << block_id.seqno << "_"
                   << hash().to_hex();
}

CandidateShort Candidate::shortref() const {
  return CandidateShort{block_id.id, hash()};
}

std::string Candidate::filename() const {
  return PSTRING() << "candidate_" << block_id.to_str() << "_" << collated_data_file_hash.to_hex() << "_"
                   << td::base64url_encode(source.export_as_slice());
}

std::string Candidate::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.id.shard));
  return PSTRING() << "candidate_" << block_id.id.workchain << "_" << s << "_" << block_id.id.seqno << "_"
                   << hash().to_hex();
}

std::string CandidateShort::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.shard));
  return PSTRING() << "candidate_" << block_id.workchain << "_" << s << "_" << block_id.seqno << "_" << hash().to_hex();
}

BlockInfoShort BlockInfo::shortref() const {
  return BlockInfoShort{block_id.id, hash()};
}

std::string BlockInfo::filename() const {
  return PSTRING() << "info_" << block_id.to_str();
}

std::string BlockInfo::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.id.shard));
  return PSTRING() << "info_" << block_id.id.workchain << "_" << s << "_" << block_id.id.seqno << "_"
                   << hash().to_hex();
}

std::string BlockInfoShort::filename_short() const {
  char s[33];
  sprintf(s, "%llx", static_cast<long long>(block_id.shard));
  return PSTRING() << "info_" << block_id.workchain << "_" << s << "_" << block_id.seqno << "_" << hash().to_hex();
}

}  // namespace fileref

FileReference::FileReference(tl_object_ptr<ton_api::db_filedb_Key> key) {
  ton_api::downcast_call(
      *key.get(),
      td::overloaded(
          [&](const ton_api::db_filedb_key_empty& key) { ref_ = fileref::Empty{}; },
          [&](const ton_api::db_filedb_key_blockFile& key) { ref_ = fileref::Block{create_block_id(key.block_id_)}; },
          [&](const ton_api::db_filedb_key_zeroStateFile& key) {
            ref_ = fileref::ZeroState{create_block_id(key.block_id_)};
          },
          [&](const ton_api::db_filedb_key_persistentStateFile& key) {
            ref_ = fileref::PersistentState{create_block_id(key.block_id_), create_block_id(key.masterchain_block_id_)};
          },
          [&](const ton_api::db_filedb_key_proof& key) { ref_ = fileref::Proof{create_block_id(key.block_id_)}; },
          [&](const ton_api::db_filedb_key_proofLink& key) {
            ref_ = fileref::ProofLink{create_block_id(key.block_id_)};
          },
          [&](const ton_api::db_filedb_key_signatures& key) {
            ref_ = fileref::Signatures{create_block_id(key.block_id_)};
          },
          [&](const ton_api::db_filedb_key_candidate& key) {
            ref_ = fileref::Candidate{PublicKey{key.id_->source_}, create_block_id(key.id_->id_),
                                      key.id_->collated_data_file_hash_};
          },
          [&](const ton_api::db_filedb_key_blockInfo& key) {
            ref_ = fileref::BlockInfo{create_block_id(key.block_id_)};
          }));
}

FileReferenceShort FileReference::shortref() const {
  FileReferenceShort h;
  ref_.visit([&](const auto& obj) { h = obj.shortref(); });
  return h;
}

td::Bits256 FileReference::hash() const {
  FileHash h;
  ref_.visit([&](const auto& obj) { h = obj.hash(); });
  return h;
}

td::Bits256 FileReferenceShort::hash() const {
  FileHash h;
  ref_.visit([&](const auto& obj) { h = obj.hash(); });
  return h;
}

ShardIdFull FileReference::shard() const {
  ShardIdFull h;
  ref_.visit([&](const auto& obj) { h = obj.shard(); });
  return h;
}

ShardIdFull FileReferenceShort::shard() const {
  ShardIdFull h;
  ref_.visit([&](const auto& obj) { h = obj.shard(); });
  return h;
}

std::string FileReference::filename() const {
  std::string h;
  ref_.visit([&](const auto& obj) { h = obj.filename(); });
  return h;
}

std::string FileReference::filename_short() const {
  std::string h;
  ref_.visit([&](const auto& obj) { h = obj.filename_short(); });
  return h;
}

std::string FileReferenceShort::filename_short() const {
  std::string h;
  ref_.visit([&](const auto& obj) { h = obj.filename_short(); });
  return h;
}

td::Result<FileReference> FileReference::create(std::string filename) {
  std::stringstream ss{filename};

  std::string token;
  std::getline(ss, token, '_');

  if (token == "empty") {
    if (ss.eof()) {
      return fileref::Empty{};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "block") {
    std::getline(ss, token, '_');
    TRY_RESULT(block_id, BlockIdExt::from_str(token));
    if (ss.eof()) {
      return fileref::Block{block_id};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "zerostate") {
    std::getline(ss, token, '_');
    TRY_RESULT(block_id, BlockIdExt::from_str(token));
    if (ss.eof()) {
      return fileref::ZeroState{block_id};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "state") {
    std::getline(ss, token, '_');
    TRY_RESULT(masterchain_block_id, BlockIdExt::from_str(token));
    std::getline(ss, token, '_');
    TRY_RESULT(block_id, BlockIdExt::from_str(token));
    if (ss.eof()) {
      return fileref::PersistentState{block_id, masterchain_block_id};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "proof") {
    std::getline(ss, token, '_');
    TRY_RESULT(block_id, BlockIdExt::from_str(token));
    if (ss.eof()) {
      return fileref::Proof{block_id};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "prooflink") {
    std::getline(ss, token, '_');
    TRY_RESULT(block_id, BlockIdExt::from_str(token));
    if (ss.eof()) {
      return fileref::ProofLink{block_id};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "signatures") {
    std::getline(ss, token, '_');
    TRY_RESULT(block_id, BlockIdExt::from_str(token));
    if (ss.eof()) {
      return fileref::Signatures{block_id};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "candidate") {
    std::getline(ss, token, '_');
    TRY_RESULT(block_id, BlockIdExt::from_str(token));
    TRY_RESULT(col_hash, get_token_hash(ss));

    std::string rem = ss.str();

    TRY_RESULT(source_s, td::base64url_decode(rem));
    TRY_RESULT(source, PublicKey::import(source_s));
    return fileref::Candidate{source, block_id, col_hash};
  } else if (token == "info") {
    std::getline(ss, token, '_');
    TRY_RESULT(block_id, BlockIdExt::from_str(token));
    if (ss.eof()) {
      return fileref::BlockInfo{block_id};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else {
    return td::Status::Error(ErrorCode::protoviolation, PSTRING() << "unknown prefix '" << token << "'");
  }
}

td::Result<FileReferenceShort> FileReferenceShort::create(std::string filename) {
  std::stringstream ss{filename};

  std::string token;
  std::getline(ss, token, '_');

  if (token == "empty") {
    if (ss.eof()) {
      return fileref::Empty{};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "block") {
    TRY_RESULT(block_id, get_block_id(ss));
    TRY_RESULT(vhash, get_token_hash(ss));
    if (ss.eof()) {
      return fileref::BlockShort{block_id, vhash};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "zerostate") {
    std::getline(ss, token, '_');
    TRY_RESULT(workchain, td::to_integer_safe<WorkchainId>(token));
    TRY_RESULT(vhash, get_token_hash(ss));
    if (ss.eof()) {
      return fileref::ZeroStateShort{workchain, vhash};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "state") {
    std::getline(ss, token, '_');
    TRY_RESULT(masterchain_seqno, td::to_integer_safe<BlockSeqno>(token));
    std::getline(ss, token, '_');
    TRY_RESULT(workchain, td::to_integer_safe<WorkchainId>(token));
    std::getline(ss, token, '_');
    TRY_RESULT(shard, td::hex_to_integer_safe<ShardId>(token));
    TRY_RESULT(vhash, get_token_hash(ss));
    if (ss.eof()) {
      return fileref::PersistentStateShort{ShardIdFull{workchain, shard}, masterchain_seqno, vhash};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "proof") {
    TRY_RESULT(block_id, get_block_id(ss));
    TRY_RESULT(vhash, get_token_hash(ss));
    if (ss.eof()) {
      return fileref::ProofShort{block_id, vhash};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "prooflink") {
    TRY_RESULT(block_id, get_block_id(ss));
    TRY_RESULT(vhash, get_token_hash(ss));
    if (ss.eof()) {
      return fileref::ProofLinkShort{block_id, vhash};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "signatures") {
    TRY_RESULT(block_id, get_block_id(ss));
    TRY_RESULT(vhash, get_token_hash(ss));
    if (ss.eof()) {
      return fileref::SignaturesShort{block_id, vhash};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "candidate") {
    TRY_RESULT(block_id, get_block_id(ss));
    TRY_RESULT(vhash, get_token_hash(ss));
    if (ss.eof()) {
      return fileref::CandidateShort{block_id, vhash};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else if (token == "info") {
    TRY_RESULT(block_id, get_block_id(ss));
    TRY_RESULT(vhash, get_token_hash(ss));
    if (ss.eof()) {
      return fileref::BlockInfoShort{block_id, vhash};
    } else {
      return td::Status::Error(ErrorCode::protoviolation, "too big file name");
    }
  } else {
    return td::Status::Error(ErrorCode::protoviolation, PSTRING() << "unknown prefix '" << token << "'");
  }
}

}  // namespace validator

}  // namespace ton
