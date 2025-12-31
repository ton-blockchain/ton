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
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api.hpp"
#include "common/errorcode.h"
#include "keys/keys.hpp"
#include "td/utils/overloaded.h"
#include "tl-utils/common-utils.hpp"
#include "tl-utils/tl-utils.hpp"
#include "ton/ton-tl.hpp"
#include "vm/cells/CellString.h"

#include "block-auto.h"
#include "mc-config.h"
#include "signature-set.h"

namespace block {

static td::Status check_vset(const BlockSignatureSet* sig_set, const td::Ref<ValidatorSet>& vset) {
  if (vset->get_catchain_seqno() != sig_set->get_catchain_seqno()) {
    return td::Status::Error(ton::ErrorCode::protoviolation, PSTRING() << "catchain seqno mismatch: expected "
                                                                       << vset->get_catchain_seqno() << ", found "
                                                                       << sig_set->get_catchain_seqno());
  }
  if (vset->get_validator_set_hash() != sig_set->get_validator_set_hash()) {
    return td::Status::Error(ton::ErrorCode::protoviolation, PSTRING() << "validator set hash mismatch: expected "
                                                                       << vset->get_validator_set_hash() << ", found "
                                                                       << sig_set->get_validator_set_hash());
  }
  return td::Status::OK();
}

static ton::tl_object_ptr<ton::ton_api::consensus_CandidateParent> clone_tl(
    const ton::tl_object_ptr<ton::ton_api::consensus_CandidateParent>& f) {
  ton::tl_object_ptr<ton::ton_api::consensus_CandidateParent> result;
  ton::ton_api::downcast_call(
      *f, td::overloaded(
              [&](const ton::ton_api::consensus_candidateParent& obj) {
                result = ton::create_tl_object<ton::ton_api::consensus_candidateParent>(
                    ton::create_tl_object<ton::ton_api::consensus_candidateId>(obj.id_->slot_, obj.id_->hash_));
              },
              [&](const ton::ton_api::consensus_candidateWithoutParents&) {
                result = ton::create_tl_object<ton::ton_api::consensus_candidateWithoutParents>();
              }));
  return result;
}

static ton::tl_object_ptr<ton::ton_api::consensus_CandidateHashData> clone_tl(
    const ton::tl_object_ptr<ton::ton_api::consensus_CandidateHashData>& f) {
  ton::tl_object_ptr<ton::ton_api::consensus_CandidateHashData> result;
  ton::ton_api::downcast_call(
      *f, td::overloaded(
              [&](const ton::ton_api::consensus_candidateHashDataOrdinary& obj) {
                result = ton::create_tl_object<ton::ton_api::consensus_candidateHashDataOrdinary>(
                    ton::create_tl_block_id(ton::create_block_id(obj.block_)), obj.collated_file_hash_,
                    clone_tl(obj.parent_));
              },
              [&](const ton::ton_api::consensus_candidateHashDataEmpty& obj) {
                result = ton::create_tl_object<ton::ton_api::consensus_candidateHashDataEmpty>(
                    ton::create_tl_block_id(ton::create_block_id(obj.block_)),
                    ton::create_tl_object<ton::ton_api::consensus_candidateId>(obj.parent_->slot_, obj.parent_->hash_));
              }));
  return result;
}

class BlockSignatureSetBase : public BlockSignatureSet {
 public:
  explicit BlockSignatureSetBase(std::vector<ton::BlockSignature> signatures, ton::CatchainSeqno cc_seqno,
                                 td::uint32 validator_set_hash)
      : BlockSignatureSet(cc_seqno, validator_set_hash), signatures_(std::move(signatures)) {
  }

  virtual td::Result<td::BufferSlice> to_sign(ton::BlockIdExt block_id) const = 0;
  virtual bool check_threshold(ton::ValidatorWeight sig_weight, ton::ValidatorWeight total_weight) const {
    return sig_weight * 3 > total_weight * 2;
  }

  size_t get_size() const override {
    return signatures_.size();
  }

  td::Result<ton::ValidatorWeight> get_weight(td::Ref<ValidatorSet> vset) const override {
    TRY_STATUS(check_vset(this, vset));
    ton::ValidatorWeight weight = 0;
    std::set<ton::NodeIdShort> nodes;
    for (auto& sig : signatures_) {
      if (nodes.contains(sig.node)) {
        return td::Status::Error(ton::ErrorCode::protoviolation, "duplicate node");
      }
      nodes.insert(sig.node);
      auto validator = vset->get_validator(sig.node);
      if (!validator) {
        return td::Status::Error(ton::ErrorCode::protoviolation, "unknown node");
      }
      weight += validator->weight;
    }
    return weight;
  }

  td::Result<td::Ref<vm::Cell>> serialize_dict() const {
    vm::Dictionary dict{16};  // HashmapE 16 CryptoSignaturePair
    for (unsigned i = 0; i < signatures_.size(); i++) {
      const ton::BlockSignature& sig = signatures_[i];
      vm::CellBuilder cb;
      if (!(cb.store_bits_bool(sig.node)                      // sig_pair$_ node_id_short:bits256
            && cb.store_long_bool(5, 4)                       //   ed25519_signature#5
            && sig.signature.size() == 64                     // signature must be 64 bytes long
            && cb.store_bytes_bool(sig.signature.data(), 64)  // R:bits256 s:bits256
            && dict.set_builder(td::BitArray<16>{i}, cb, vm::Dictionary::SetMode::Add))) {
        return td::Status::Error(PSTRING() << "failed to serialize");
      }
    }
    return std::move(dict).extract_root_cell();
  }

 protected:
  std::vector<ton::BlockSignature> signatures_;

  td::Result<ton::ValidatorWeight> check_signatures_impl(td::Ref<ValidatorSet> vset,
                                                         ton::BlockIdExt block_id) const override {
    TRY_STATUS(check_vset(this, vset));
    TRY_RESULT(data, to_sign(block_id));
    ton::ValidatorWeight weight = 0;
    std::set<ton::NodeIdShort> nodes;
    for (auto& sig : signatures_) {
      if (nodes.contains(sig.node)) {
        return td::Status::Error(ton::ErrorCode::protoviolation, "duplicate node");
      }
      nodes.insert(sig.node);

      auto validator = vset->get_validator(sig.node);
      if (!validator) {
        return td::Status::Error(ton::ErrorCode::protoviolation, "unknown node");
      }

      auto E = ton::PublicKey{ton::pubkeys::Ed25519{validator->key}}.create_encryptor().move_as_ok();
      TRY_STATUS(E->check_signature(data, sig.signature.as_slice()));
      weight += validator->weight;
    }

    if (!check_threshold(weight, vset->get_total_weight())) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "too small sig weight");
    }
    return weight;
  }
};

class BlockSignatureSetOrdinary : public BlockSignatureSetBase {
 public:
  explicit BlockSignatureSetOrdinary(std::vector<ton::BlockSignature> signatures, ton::CatchainSeqno cc_seqno,
                                     td::uint32 validator_set_hash)
      : BlockSignatureSetBase(std::move(signatures), cc_seqno, validator_set_hash) {
  }
  ~BlockSignatureSetOrdinary() override = default;

  CntObject* make_copy() const override {
    std::vector<ton::BlockSignature> copy;
    for (const auto& s : signatures_) {
      copy.emplace_back(s.node, s.signature.clone());
    }
    return new BlockSignatureSetOrdinary(std::move(copy), cc_seqno_, validator_set_hash_);
  }

  bool is_ordinary() const override {
    return true;
  }
  bool is_final() const override {
    return true;
  }

  td::Result<td::BufferSlice> to_sign(ton::BlockIdExt block_id) const override {
    return ton::create_serialize_tl_object<ton::ton_api::ton_blockId>(block_id.root_hash, block_id.file_hash);
  }

  ton::tl_object_ptr<ton::ton_api::tonNode_SignatureSet> tl() const override {
    auto f = ton::create_tl_object<ton::ton_api::tonNode_signatureSet_ordinary>();
    f->cc_seqno_ = cc_seqno_;
    f->validator_set_hash_ = validator_set_hash_;
    for (auto& sig : signatures_) {
      f->signatures_.push_back(
          ton::create_tl_object<ton::ton_api::tonNode_blockSignature>(sig.node, sig.signature.clone()));
    }
    return f;
  }

  ton::tl_object_ptr<ton::lite_api::liteServer_SignatureSet> tl_lite() const override {
    auto f = ton::create_tl_object<ton::lite_api::liteServer_signatureSet_ordinary>();
    f->catchain_seqno_ = cc_seqno_;
    f->validator_set_hash_ = validator_set_hash_;
    for (auto& sig : signatures_) {
      f->signatures_.push_back(
          ton::create_tl_object<ton::lite_api::liteServer_signature>(sig.node, sig.signature.clone()));
    }
    return f;
  }

  std::vector<ton::tl_object_ptr<ton::ton_api::tonNode_blockSignature>> tl_legacy() const override {
    std::vector<ton::tl_object_ptr<ton::ton_api::tonNode_blockSignature>> f;
    for (auto& sig : signatures_) {
      f.push_back(ton::create_tl_object<ton::ton_api::tonNode_blockSignature>(sig.node, sig.signature.clone()));
    }
    return f;
  }

  td::Result<td::Ref<vm::Cell>> serialize(td::Ref<ValidatorSet> vset) const override {
    TRY_RESULT(weight, get_weight(vset));
    TRY_RESULT(dict_root, serialize_dict());
    // block_signatures_ordinary#11 validator_list_hash_short:uint32 catchain_seqno:uint32
    //   sig_count:uint32 sig_weight:uint64
    //   signatures:(HashmapE 16 CryptoSignaturePair) = BlockSignatures;
    vm::CellBuilder cb;
    cb.store_long(0x11, 8);
    cb.store_long(validator_set_hash_, 32);
    cb.store_long(cc_seqno_, 32);
    cb.store_long(signatures_.size(), 32);
    cb.store_long(weight, 64);
    cb.store_maybe_ref(dict_root);
    return cb.finalize_novm();
  }
};

class BlockSignatureSetSimplex : public BlockSignatureSetBase {
 public:
  explicit BlockSignatureSetSimplex(std::vector<ton::BlockSignature> signatures, ton::CatchainSeqno cc_seqno,
                                    td::uint32 validator_set_hash, td::Bits256 session_id, td::uint32 slot,
                                    ton::tl_object_ptr<ton::ton_api::consensus_CandidateHashData> candidate, bool final)
      : BlockSignatureSetBase(std::move(signatures), cc_seqno, validator_set_hash)
      , session_id_(session_id)
      , slot_(slot)
      , candidate_(std::move(candidate))
      , final_(final) {
  }
  ~BlockSignatureSetSimplex() override = default;

  CntObject* make_copy() const override {
    std::vector<ton::BlockSignature> copy;
    for (const auto& s : signatures_) {
      copy.emplace_back(s.node, s.signature.clone());
    }
    return new BlockSignatureSetSimplex(std::move(copy), cc_seqno_, validator_set_hash_, session_id_, slot_,
                                        clone_tl(candidate_), final_);
  }

  bool is_final() const override {
    return final_;
  }

  td::Result<td::BufferSlice> to_sign(ton::BlockIdExt block_id) const override {
    ton::BlockIdExt expected_block_id;
    ton::ton_api::downcast_call(*candidate_, td::overloaded(
                                                 [&](const ton::ton_api::consensus_candidateHashDataOrdinary& obj) {
                                                   expected_block_id = ton::create_block_id(obj.block_);
                                                 },
                                                 [&](const ton::ton_api::consensus_candidateHashDataEmpty& obj) {
                                                   expected_block_id = ton::create_block_id(obj.block_);
                                                 }));
    if (block_id != expected_block_id) {
      return td::Status::Error("block id mismatch");
    }
    auto candidate_id = ton::create_tl_object<ton::ton_api::consensus_candidateId>(
        slot_, td::Bits256{ton::get_tl_object_sha256(candidate_).raw});
    td::BufferSlice data;
    if (final_) {
      data = ton::create_serialize_tl_object<ton::ton_api::consensus_simplex_finalizeVote>(std::move(candidate_id));
    } else {
      data = ton::create_serialize_tl_object<ton::ton_api::consensus_simplex_notarizeVote>(std::move(candidate_id));
    }
    return ton::create_serialize_tl_object<ton::ton_api::consensus_dataToSign>(session_id_, std::move(data));
  }

  ton::tl_object_ptr<ton::ton_api::tonNode_SignatureSet> tl() const override {
    auto f = ton::create_tl_object<ton::ton_api::tonNode_signatureSet_simplex>();
    f->cc_seqno_ = cc_seqno_;
    f->validator_set_hash_ = validator_set_hash_;
    for (auto& sig : signatures_) {
      f->signatures_.push_back(
          ton::create_tl_object<ton::ton_api::tonNode_blockSignature>(sig.node, sig.signature.clone()));
    }
    f->session_id_ = session_id_;
    f->slot_ = slot_;
    f->candidate_ = clone_tl(candidate_);
    f->final_ = final_;
    return f;
  }

  ton::tl_object_ptr<ton::lite_api::liteServer_SignatureSet> tl_lite() const override {
    CHECK(final_);
    auto f = ton::create_tl_object<ton::lite_api::liteServer_signatureSet_simplex>();
    f->cc_seqno_ = cc_seqno_;
    f->validator_set_hash_ = validator_set_hash_;
    for (auto& sig : signatures_) {
      f->signatures_.push_back(
          ton::create_tl_object<ton::lite_api::liteServer_signature>(sig.node, sig.signature.clone()));
    }
    f->session_id_ = session_id_;
    f->slot_ = slot_;
    f->candidate_ = ton::serialize_tl_object(candidate_, true);
    return f;
  }

  td::Result<td::Ref<vm::Cell>> serialize(td::Ref<ValidatorSet> vset) const override {
    if (!final_) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "cannot serialize approve simplex signatures to cell");
    }
    TRY_RESULT(weight, get_weight(vset));
    TRY_RESULT(dict_root, serialize_dict());
    // block_signatures_simplex#12 validator_list_hash_short:uint32 catchain_seqno:uint32
    //   sig_count:uint32 sig_weight:uint64
    //   signatures:(HashmapE 16 CryptoSignaturePair)
    //   session_id:bits256 slot:uint32 candidate_data:^Cell = BlockSignatures;
    vm::CellBuilder cb;
    cb.store_long(0x12, 8);
    cb.store_long(validator_set_hash_, 32);
    cb.store_long(cc_seqno_, 32);
    cb.store_long(signatures_.size(), 32);
    cb.store_long(weight, 64);
    cb.store_maybe_ref(dict_root);
    cb.store_bytes(session_id_.as_slice());
    cb.store_long(slot_, 32);
    TRY_RESULT(candidate_cell, vm::CellString::create(ton::serialize_tl_object(candidate_, true)));
    cb.store_ref(candidate_cell);
    return cb.finalize_novm();
  }

 private:
  td::Bits256 session_id_;
  td::uint32 slot_;
  ton::tl_object_ptr<ton::ton_api::consensus_CandidateHashData> candidate_;
  bool final_;
};

td::Ref<BlockSignatureSet> BlockSignatureSet::create_ordinary(std::vector<ton::BlockSignature> signatures,
                                                              ton::CatchainSeqno cc_seqno,
                                                              td::uint32 validator_set_hash) {
  return td::Ref<BlockSignatureSetOrdinary>{true, std::move(signatures), cc_seqno, validator_set_hash};
}

td::Ref<BlockSignatureSet> BlockSignatureSet::create_simplex(
    std::vector<ton::BlockSignature> signatures, ton::CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
    td::Bits256 session_id, td::uint32 slot, ton::tl_object_ptr<ton::ton_api::consensus_CandidateHashData> candidate) {
  return td::Ref<BlockSignatureSetSimplex>{true, std::move(signatures), cc_seqno, validator_set_hash, session_id,
                                           slot, std::move(candidate),  true};
}

td::Ref<BlockSignatureSet> BlockSignatureSet::create_simplex_approve(
    std::vector<ton::BlockSignature> signatures, ton::CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
    td::Bits256 session_id, td::uint32 slot, ton::tl_object_ptr<ton::ton_api::consensus_CandidateHashData> candidate) {
  return td::Ref<BlockSignatureSetSimplex>{true, std::move(signatures), cc_seqno, validator_set_hash, session_id,
                                           slot, std::move(candidate),  false};
}

static td::Result<std::vector<ton::BlockSignature>> unpack_signatures_dict(td::Ref<vm::Cell> dict_root) {
  std::vector<ton::BlockSignature> signatures;
  vm::Dictionary dict{dict_root, 16};  // HashmapE 16 CryptoSignaturePair
  unsigned i = 0;
  if (!dict.check_for_each([&](Ref<vm::CellSlice> cs_ref, td::ConstBitPtr key, int n) -> bool {
        if (key.get_int(n) != i || cs_ref->size_ext() != 256 + 4 + 256 + 256) {
          return false;
        }
        vm::CellSlice cs{*cs_ref};
        ton::NodeIdShort node_id;
        unsigned char signature[64];
        if (!(cs.fetch_bits_to(node_id)         // sig_pair$_ node_id_short:bits256
              && cs.fetch_ulong(4) == 5         // ed25519_signature#5
              && cs.fetch_bytes(signature, 64)  // R:bits256 s:bits256
              && !cs.size_ext())) {
          return false;
        }
        signatures.emplace_back(node_id, td::BufferSlice{td::Slice{signature, 64}});
        ++i;
        return i <= BlockSignatureSet::MAX_SIGNATURES;
      })) {
    return td::Status::Error("failed to parse signatures dict");
  }
  return signatures;
}

td::Result<td::Ref<BlockSignatureSet>> BlockSignatureSet::fetch(td::Ref<vm::Cell> cell,
                                                                ton::ValidatorWeight& total_weight) {
  if (cell.is_null()) {
    return td::Status::Error("cell is null");
  }
  try {
    if (gen::BlockSignatures::Record_block_signatures_ordinary rec; gen::unpack_cell(cell, rec)) {
      TRY_RESULT(signatures, unpack_signatures_dict(rec.signatures->prefetch_ref()));
      auto sig_set = create_ordinary(std::move(signatures), rec.catchain_seqno, rec.validator_list_hash_short);
      if (sig_set->get_size() != rec.sig_count) {
        return td::Status::Error("signature count mismatch");
      }
      total_weight = rec.sig_weight;
      return sig_set;
    }
    if (gen::BlockSignatures::Record_block_signatures_simplex rec; gen::unpack_cell(cell, rec)) {
      TRY_RESULT(signatures, unpack_signatures_dict(rec.signatures->prefetch_ref()));
      vm::CellSlice candidate_cs = vm::load_cell_slice(rec.candidate_data);
      TRY_RESULT(candidate_data, vm::CellString::load(candidate_cs));
      TRY_RESULT(candidate, ton::fetch_tl_object<ton::ton_api::consensus_CandidateHashData>(candidate_data, true));
      auto sig_set = create_simplex(std::move(signatures), rec.catchain_seqno, rec.validator_list_hash_short,
                                    rec.session_id, rec.slot, std::move(candidate));
      if (sig_set->get_size() != rec.sig_count) {
        return td::Status::Error("signature count mismatch");
      }
      total_weight = rec.sig_weight;
      return sig_set;
    }
    return td::Status::Error("failed to unpack signature set");
  } catch (vm::VmError& e) {
    return e.as_status();
  }
}

td::Result<td::Ref<BlockSignatureSet>> BlockSignatureSet::fetch(td::Ref<vm::Cell> cell, td::Ref<ValidatorSet> vset) {
  ton::ValidatorWeight total_weight;
  TRY_RESULT(sig_set, fetch(std::move(cell), total_weight));
  TRY_RESULT(expected_weight, sig_set->get_weight(vset));
  if (expected_weight != total_weight) {
    return td::Status::Error("signature weight mismatch");
  }
  return sig_set;
}

td::Ref<BlockSignatureSet> BlockSignatureSet::fetch(
    const std::vector<ton::tl_object_ptr<ton::ton_api::tonNode_blockSignature>>& f, ton::CatchainSeqno cc_seqno,
    td::uint32 validator_set_hash) {
  std::vector<ton::BlockSignature> signatures;
  for (auto& s : f) {
    signatures.emplace_back(s->who_, s->signature_.clone());
  }
  return create_ordinary(std::move(signatures), cc_seqno, validator_set_hash);
}

td::Ref<BlockSignatureSet> BlockSignatureSet::fetch(const ton::tl_object_ptr<ton::ton_api::tonNode_SignatureSet>& f) {
  td::Ref<BlockSignatureSet> sig_set;
  ton::ton_api::downcast_call(
      *f, td::overloaded(
              [&](const ton::ton_api::tonNode_signatureSet_ordinary& obj) {
                std::vector<ton::BlockSignature> signatures;
                for (auto& s : obj.signatures_) {
                  signatures.emplace_back(s->who_, s->signature_.clone());
                }
                sig_set = create_ordinary(std::move(signatures), obj.cc_seqno_, obj.validator_set_hash_);
              },
              [&](const ton::ton_api::tonNode_signatureSet_simplex& obj) {
                std::vector<ton::BlockSignature> signatures;
                for (auto& s : obj.signatures_) {
                  signatures.emplace_back(s->who_, s->signature_.clone());
                }
                sig_set = td::Ref<BlockSignatureSetSimplex>(true, std::move(signatures), obj.cc_seqno_,
                                                            obj.validator_set_hash_, obj.session_id_, obj.slot_,
                                                            clone_tl(obj.candidate_), obj.final_);
              }));
  return sig_set;
}

td::Result<td::Ref<BlockSignatureSet>> BlockSignatureSet::fetch(
    const ton::tl_object_ptr<ton::lite_api::liteServer_SignatureSet>& f) {
  td::Result<td::Ref<BlockSignatureSet>> sig_set;
  ton::lite_api::downcast_call(
      *f, td::overloaded(
              [&](const ton::lite_api::liteServer_signatureSet_ordinary& obj) {
                std::vector<ton::BlockSignature> signatures;
                for (auto& s : obj.signatures_) {
                  signatures.emplace_back(s->node_id_short_, s->signature_.clone());
                }
                sig_set = create_ordinary(std::move(signatures), obj.catchain_seqno_, obj.validator_set_hash_);
              },
              [&](const ton::lite_api::liteServer_signatureSet_simplex& obj) {
                std::vector<ton::BlockSignature> signatures;
                for (auto& s : obj.signatures_) {
                  signatures.emplace_back(s->node_id_short_, s->signature_.clone());
                }
                auto r_candidate =
                    ton::fetch_tl_object<ton::ton_api::consensus_CandidateHashData>(obj.candidate_, true);
                if (r_candidate.is_error()) {
                  sig_set = r_candidate.move_as_error_prefix("failed to unpack candidate data: ");
                  return;
                }
                sig_set = create_simplex(std::move(signatures), obj.cc_seqno_, obj.validator_set_hash_, obj.session_id_,
                                         obj.slot_, r_candidate.move_as_ok());
              }));
  return sig_set;
}

}  // namespace block
