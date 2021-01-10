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

#include "ton/ton-types.h"
#include "td/actor/actor.h"
#include "validator/interfaces/shard.h"
#include "td/db/KeyValue.h"
#include "ton/ton-tl.hpp"

namespace ton {

namespace validator {

namespace fileref {

class Empty {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_empty>();
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_empty>();
  }
  ShardIdFull shard() const {
    return ShardIdFull{masterchainId};
  }
  std::string filename() const;
  std::string filename_short() const;
  Empty shortref() const;
};

class BlockShort {
 public:
  FileHash hash() const {
    return hashv;
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  std::string filename_short() const;

  BlockId block_id;
  FileHash hashv;
};

class Block {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_blockFile>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_blockFile>(create_tl_block_id(block_id));
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  BlockShort shortref() const;
  std::string filename() const;
  std::string filename_short() const;

  BlockIdExt block_id;
};

class ZeroStateShort {
 public:
  FileHash hash() const {
    return hashv;
  }
  ShardIdFull shard() const {
    return ShardIdFull{workchain, shardIdAll};
  }
  std::string filename_short() const;

  WorkchainId workchain;
  FileHash hashv;
};

class ZeroState {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_zeroStateFile>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_zeroStateFile>(create_tl_block_id(block_id));
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  ZeroStateShort shortref() const;
  std::string filename() const;
  std::string filename_short() const;

  BlockIdExt block_id;
};

class PersistentStateShort {
 public:
  FileHash hash() const {
    return hashv;
  }
  ShardIdFull shard() const {
    return shard_id;
  }
  std::string filename_short() const;

  ShardIdFull shard_id;
  BlockSeqno masterchain_seqno;
  FileHash hashv;
};

class PersistentState {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_persistentStateFile>(create_tl_block_id(block_id),
                                                                        create_tl_block_id(masterchain_block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_persistentStateFile>(create_tl_block_id(block_id),
                                                                             create_tl_block_id(masterchain_block_id));
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  PersistentStateShort shortref() const;
  std::string filename() const;
  std::string filename_short() const;

  BlockIdExt block_id;
  BlockIdExt masterchain_block_id;
};

class ProofShort {
 public:
  FileHash hash() const {
    return hashv;
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  std::string filename_short() const;

  BlockId block_id;
  FileHash hashv;
};

class Proof {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_proof>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_proof>(create_tl_block_id(block_id));
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  ProofShort shortref() const;
  std::string filename() const;
  std::string filename_short() const;

  BlockIdExt block_id;
};

class ProofLinkShort {
 public:
  FileHash hash() const {
    return hashv;
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  std::string filename_short() const;

  BlockId block_id;
  FileHash hashv;
};

class ProofLink {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_proofLink>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_proofLink>(create_tl_block_id(block_id));
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  ProofLinkShort shortref() const;
  std::string filename() const;
  std::string filename_short() const;

  BlockIdExt block_id;
};

class SignaturesShort {
 public:
  FileHash hash() const {
    return hashv;
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  std::string filename_short() const;

  BlockId block_id;
  FileHash hashv;
};

class Signatures {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_signatures>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_signatures>(create_tl_block_id(block_id));
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  SignaturesShort shortref() const;
  std::string filename() const;
  std::string filename_short() const;

  BlockIdExt block_id;
};

class CandidateShort {
 public:
  FileHash hash() const {
    return hashv;
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  std::string filename_short() const;

  BlockId block_id;
  FileHash hashv;
};

class Candidate {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_candidate>(
        create_tl_object<ton_api::db_candidate_id>(source.tl(), create_tl_block_id(block_id), collated_data_file_hash));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_candidate>(
        create_tl_object<ton_api::db_candidate_id>(source.tl(), create_tl_block_id(block_id), collated_data_file_hash));
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  CandidateShort shortref() const;
  std::string filename() const;
  std::string filename_short() const;

  PublicKey source;
  BlockIdExt block_id;
  FileHash collated_data_file_hash;
};

class BlockInfoShort {
 public:
  FileHash hash() const {
    return hashv;
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  std::string filename_short() const;

  BlockId block_id;
  FileHash hashv;
};

class BlockInfo {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_blockInfo>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_blockInfo>(create_tl_block_id(block_id));
  }
  ShardIdFull shard() const {
    return block_id.shard_full();
  }
  BlockInfoShort shortref() const;
  std::string filename() const;
  std::string filename_short() const;

  BlockIdExt block_id;
};

};  // namespace fileref

class FileReferenceShort {
 private:
  td::Variant<fileref::Empty, fileref::BlockShort, fileref::ZeroStateShort, fileref::PersistentStateShort,
              fileref::ProofShort, fileref::ProofShort, fileref::ProofLinkShort, fileref::SignaturesShort,
              fileref::CandidateShort, fileref::BlockInfoShort>
      ref_;

 public:
  template <typename T>
  FileReferenceShort(T x) : ref_(std::move(x)) {
  }
  FileReferenceShort() : ref_(fileref::Empty{}) {
  }

  static td::Result<FileReferenceShort> create(std::string filename);

  auto &ref() {
    return ref_;
  }

  td::Bits256 hash() const;
  ShardIdFull shard() const;
  std::string filename_short() const;
};

class FileReference {
 private:
  td::Variant<fileref::Empty, fileref::Block, fileref::ZeroState, fileref::PersistentState, fileref::Proof,
              fileref::Proof, fileref::ProofLink, fileref::Signatures, fileref::Candidate, fileref::BlockInfo>
      ref_;

 public:
  template <typename T>
  FileReference(T x) : ref_(std::move(x)) {
  }
  FileReference() : ref_(fileref::Empty{}) {
  }
  FileReference(tl_object_ptr<ton_api::db_filedb_Key> key);

  static td::Result<FileReference> create(std::string filename);

  auto &ref() {
    return ref_;
  }

  FileReferenceShort shortref() const;

  tl_object_ptr<ton_api::db_filedb_Key> tl() const;
  td::Bits256 hash() const;
  ShardIdFull shard() const;
  std::string filename() const;
  std::string filename_short() const;
};

}  // namespace validator

}  // namespace ton

