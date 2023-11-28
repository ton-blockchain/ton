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

#include "p256.h"
#include "td/utils/check.h"
#include "td/utils/misc.h"
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <iostream>

namespace td {

td::Status p256_check_signature(td::Slice data, td::Slice public_key, td::Slice signature) {
  CHECK(public_key.size() == 33);
  CHECK(signature.size() == 64);

  EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  if (pctx == nullptr) {
    return td::Status::Error("Can't create EVP_PKEY_CTX");
  }
  SCOPE_EXIT {
    EVP_PKEY_CTX_free(pctx);
  };
  if (EVP_PKEY_paramgen_init(pctx) <= 0) {
    return td::Status::Error("EVP_PKEY_paramgen_init failed");
  }
  if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0) {
    return td::Status::Error("EVP_PKEY_CTX_set_ec_paramgen_curve_nid failed");
  }
  EVP_PKEY* pkey = nullptr;
  if (EVP_PKEY_paramgen(pctx, &pkey) <= 0) {
    return td::Status::Error("EVP_PKEY_paramgen failed");
  }
  SCOPE_EXIT {
    EVP_PKEY_free(pkey);
  };
  if (EVP_PKEY_set1_tls_encodedpoint(pkey, public_key.ubegin(), public_key.size()) <= 0) {
    return td::Status::Error("Failed to import public key");
  }
  EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
  if (md_ctx == nullptr) {
    return td::Status::Error("Can't create EVP_MD_CTX");
  }
  SCOPE_EXIT {
    EVP_MD_CTX_free(md_ctx);
  };
  if (EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, pkey) <= 0) {
    return td::Status::Error("Can't init DigestVerify");
  }
  ECDSA_SIG* sig = ECDSA_SIG_new();
  SCOPE_EXIT {
    ECDSA_SIG_free(sig);
  };
  unsigned char buf[33];
  buf[0] = 0;
  std::copy(signature.ubegin(), signature.ubegin() + 32, buf + 1);
  BIGNUM* r = BN_bin2bn(buf, 33, nullptr);
  std::copy(signature.ubegin() + 32, signature.ubegin() + 64, buf + 1);
  BIGNUM* s = BN_bin2bn(buf, 33, nullptr);
  if (ECDSA_SIG_set0(sig, r, s) != 1) {
    return td::Status::Error("Invalid signature");
  }
  unsigned char* signature_encoded = nullptr;
  int signature_len = i2d_ECDSA_SIG(sig, &signature_encoded);
  if (signature_len <= 0) {
    return td::Status::Error("Invalid signature");
  }
  SCOPE_EXIT {
    OPENSSL_free(signature_encoded);
  };
  if (EVP_DigestVerify(md_ctx, signature_encoded, signature_len, data.ubegin(), data.size()) == 1) {
    return td::Status::OK();
  }
  return td::Status::Error("Wrong signature");
}

}  // namespace td
