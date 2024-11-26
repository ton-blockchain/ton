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

#include "secp256k1.h"
#include "td/utils/check.h"
#include "td/utils/logging.h"

#include <secp256k1_recovery.h>
#include <secp256k1_extrakeys.h>
#include <cstring>

namespace td::secp256k1 {

static const secp256k1_context* get_context() {
  static secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
  LOG_CHECK(ctx) << "Failed to create secp256k1_context";
  return ctx;
}

bool ecrecover(const unsigned char* hash, const unsigned char* signature, unsigned char* public_key) {
  const secp256k1_context* ctx = get_context();
  secp256k1_ecdsa_recoverable_signature ecdsa_signature;
  if (signature[64] > 3 ||
      !secp256k1_ecdsa_recoverable_signature_parse_compact(ctx, &ecdsa_signature, signature, signature[64])) {
    return false;
  }
  secp256k1_pubkey pubkey;
  if (!secp256k1_ecdsa_recover(ctx, &pubkey, &ecdsa_signature, hash)) {
    return false;
  }
  size_t len = 65;
  secp256k1_ec_pubkey_serialize(ctx, public_key, &len, &pubkey, SECP256K1_EC_UNCOMPRESSED);
  CHECK(len == 65);
  return true;
}

bool xonly_pubkey_tweak_add(const unsigned char* xonly_pubkey_bytes, const unsigned char* tweak,
                            unsigned char* output_pubkey_bytes) {
  const secp256k1_context* ctx = get_context();

  secp256k1_xonly_pubkey xonly_pubkey;
  secp256k1_pubkey output_pubkey;
  if (!secp256k1_xonly_pubkey_parse(ctx, &xonly_pubkey, xonly_pubkey_bytes)) {
    return false;
  }
  if (!secp256k1_xonly_pubkey_tweak_add(ctx, &output_pubkey, &xonly_pubkey, tweak)) {
    return false;
  }
  size_t len = 65;
  secp256k1_ec_pubkey_serialize(ctx, output_pubkey_bytes, &len, &output_pubkey, SECP256K1_EC_UNCOMPRESSED);
  CHECK(len == 65);
  return true;
}

}  // namespace td::secp256k1
