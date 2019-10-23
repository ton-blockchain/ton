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

    Copyright 2017-2019 Telegram Systems LLP
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
};

class Block {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_blockFile>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_blockFile>(create_tl_block_id(block_id));
  }

  BlockIdExt block_id;
};

class ZeroState {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_zeroStateFile>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_zeroStateFile>(create_tl_block_id(block_id));
  }

  BlockIdExt block_id;
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

  BlockIdExt block_id;
  BlockIdExt masterchain_block_id;
};

class Proof {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_proof>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_proof>(create_tl_block_id(block_id));
  }

  BlockIdExt block_id;
};

class ProofLink {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_proofLink>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_proofLink>(create_tl_block_id(block_id));
  }

  BlockIdExt block_id;
};

class Signatures {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_signatures>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_signatures>(create_tl_block_id(block_id));
  }

  BlockIdExt block_id;
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

  PublicKey source;
  BlockIdExt block_id;
  FileHash collated_data_file_hash;
};

class BlockInfo {
 public:
  tl_object_ptr<ton_api::db_filedb_Key> tl() const {
    return create_tl_object<ton_api::db_filedb_key_blockInfo>(create_tl_block_id(block_id));
  }
  FileHash hash() const {
    return create_hash_tl_object<ton_api::db_filedb_key_blockInfo>(create_tl_block_id(block_id));
  }

  BlockIdExt block_id;
};
};  // namespace fileref

class RootDb;

class FileDb : public td::actor::Actor {
 public:
  using RefId =
      td::Variant<fileref::Empty, fileref::Block, fileref::ZeroState, fileref::PersistentState, fileref::Proof,
                  fileref::Proof, fileref::ProofLink, fileref::Signatures, fileref::Candidate, fileref::BlockInfo>;
  using RefIdHash = td::Bits256;

  void store_file(RefId ref_id, td::BufferSlice data, td::Promise<FileHash> promise);
  void store_file_continue(RefId ref_id, FileHash file_hash, std::string path, td::Promise<FileHash> promise);
  void load_file(RefId ref_id, td::Promise<td::BufferSlice> promise);
  void load_file_slice(RefId ref_id, td::int64 offset, td::int64 max_size, td::Promise<td::BufferSlice> promise);
  void check_file(RefId ref_id, td::Promise<bool> promise);

  void prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise);

  void start_up() override;
  void alarm() override;

  void gc();
  void skip_gc();

  FileDb(td::actor::ActorId<RootDb> root_db, std::string root_path, td::uint32 depth, bool is_archive);

 private:
  struct DbEntry {
    RefId key;
    RefIdHash prev;
    RefIdHash next;
    FileHash file_hash;

    DbEntry(tl_object_ptr<ton_api::db_filedb_value> entry);
    DbEntry() {
    }
    DbEntry(RefId key, RefIdHash prev, RefIdHash next, FileHash file_hash)
        : key(std::move(key)), prev(prev), next(next), file_hash(file_hash) {
    }
    td::BufferSlice release();
    bool is_empty() const;
  };

  static RefIdHash get_ref_id_hash(const RefId& ref);
  static tl_object_ptr<ton_api::db_filedb_Key> get_ref_id_tl(const RefId& ref);
  static RefId get_ref_from_tl(const ton_api::db_filedb_Key& from);
  static RefId get_empty_ref_id();
  RefIdHash get_empty_ref_id_hash();

  std::string get_file_name(const RefId& ref, bool create_dirs);
  td::Slice get_key(const RefIdHash& ref);

  td::Result<DbEntry> get_block(const RefIdHash& ref_id);
  void set_block(const RefIdHash& ref_id_hash, DbEntry entry);

  td::actor::ActorId<RootDb> root_db_;

  std::string root_path_;
  std::string db_path_;
  td::uint32 depth_;

  bool is_archive_;

  std::shared_ptr<td::KeyValue> kv_;

  RefIdHash last_gc_;
  RefIdHash empty_ = RefIdHash::zero();
};

}  // namespace validator

}  // namespace ton

