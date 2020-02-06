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
#include "SmartContractCode.h"

#include "vm/boc.h"
#include <map>

#include "td/utils/base64.h"

namespace ton {
namespace {
constexpr static int WALLET_REVISION = 2;
constexpr static int WALLET2_REVISION = 2;
constexpr static int WALLET3_REVISION = 2;
constexpr static int HIGHLOAD_WALLET_REVISION = 2;
constexpr static int HIGHLOAD_WALLET2_REVISION = 2;
const auto& get_map() {
  static auto map = [] {
    std::map<std::string, td::Ref<vm::Cell>, std::less<>> map;
    auto with_tvm_code = [&](auto name, td::Slice code_str) {
      map[name] = vm::std_boc_deserialize(td::base64_decode(code_str).move_as_ok()).move_as_ok();
    };
#include "smartcont/auto/multisig-code.cpp"
#include "smartcont/auto/simple-wallet-ext-code.cpp"
#include "smartcont/auto/simple-wallet-code.cpp"
#include "smartcont/auto/wallet-code.cpp"
#include "smartcont/auto/highload-wallet-code.cpp"
#include "smartcont/auto/highload-wallet-v2-code.cpp"
#include "smartcont/auto/dns-manual-code.cpp"

    with_tvm_code("highload-wallet-r1",
                  "te6ccgEBBgEAhgABFP8A9KQT9KDyyAsBAgEgAgMCAUgEBQC88oMI1xgg0x/TH9Mf+CMTu/Jj7UTQ0x/TH9P/"
                  "0VEyuvKhUUS68qIE+QFUEFX5EPKj9ATR+AB/jhghgBD0eG+hb6EgmALTB9QwAfsAkTLiAbPmWwGkyMsfyx/L/"
                  "8ntVAAE0DAAEaCZL9qJoa4WPw==");
    with_tvm_code("highload-wallet-r2",
                  "te6ccgEBCAEAmQABFP8A9KQT9LzyyAsBAgEgAgMCAUgEBQC88oMI1xgg0x/TH9Mf+CMTu/Jj7UTQ0x/TH9P/"
                  "0VEyuvKhUUS68qIE+QFUEFX5EPKj9ATR+AB/jhghgBD0eG+hb6EgmALTB9QwAfsAkTLiAbPmWwGkyMsfyx/L/"
                  "8ntVAAE0DACAUgGBwAXuznO1E0NM/MdcL/4ABG4yX7UTQ1wsfg=");
    with_tvm_code("highload-wallet-v2-r1",
                  "te6ccgEBBwEA1gABFP8A9KQT9KDyyAsBAgEgAgMCAUgEBQHu8oMI1xgg0x/TP/gjqh9TILnyY+1E0NMf0z/T//"
                  "QE0VNggED0Dm+hMfJgUXO68qIH+QFUEIf5EPKjAvQE0fgAf44YIYAQ9HhvoW+"
                  "hIJgC0wfUMAH7AJEy4gGz5luDJaHIQDSAQPRDiuYxyBLLHxPLP8v/9ADJ7VQGAATQMABBoZfl2omhpj5jpn+n/"
                  "mPoCaKkQQCB6BzfQmMktv8ld0fFADgggED0lm+hb6EyURCUMFMDud4gkzM2AZIyMOKz");
    with_tvm_code("highload-wallet-v2-r2",
                  "te6ccgEBCQEA6QABFP8A9KQT9LzyyAsBAgEgAgMCAUgEBQHu8oMI1xgg0x/TP/gjqh9TILnyY+1E0NMf0z/T//"
                  "QE0VNggED0Dm+hMfJgUXO68qIH+QFUEIf5EPKjAvQE0fgAf44YIYAQ9HhvoW+"
                  "hIJgC0wfUMAH7AJEy4gGz5luDJaHIQDSAQPRDiuYxyBLLHxPLP8v/9ADJ7VQIAATQMAIBIAYHABe9nOdqJoaa+Y64X/"
                  "wAQb5fl2omhpj5jpn+n/mPoCaKkQQCB6BzfQmMktv8ld0fFAA4IIBA9JZvoW+hMlEQlDBTA7neIJMzNgGSMjDisw==");
    with_tvm_code("simple-wallet-r1",
                  "te6ccgEEAQEAAAAAUwAAov8AIN0gggFMl7qXMO1E0NcLH+Ck8mCBAgDXGCDXCx/tRNDTH9P/"
                  "0VESuvKhIvkBVBBE+RDyovgAAdMfMSDXSpbTB9QC+wDe0aTIyx/L/8ntVA==");
    with_tvm_code("simple-wallet-r2",
                  "te6ccgEBAQEAXwAAuv8AIN0gggFMl7ohggEznLqxnHGw7UTQ0x/XC//jBOCk8mCBAgDXGCDXCx/tRNDTH9P/"
                  "0VESuvKhIvkBVBBE+RDyovgAAdMfMSDXSpbTB9QC+wDe0aTIyx/L/8ntVA==");
    with_tvm_code("wallet-r1",
                  "te6ccgEBAQEAVwAAqv8AIN0gggFMl7qXMO1E0NcLH+Ck8mCDCNcYINMf0x8B+CO78mPtRNDTH9P/0VExuvKhA/"
                  "kBVBBC+RDyovgAApMg10qW0wfUAvsA6NGkyMsfy//J7VQ=");
    with_tvm_code("wallet-r2",
                  "te6ccgEBAQEAYwAAwv8AIN0gggFMl7ohggEznLqxnHGw7UTQ0x/XC//jBOCk8mCDCNcYINMf0x8B+CO78mPtRNDTH9P/"
                  "0VExuvKhA/kBVBBC+RDyovgAApMg10qW0wfUAvsA6NGkyMsfy//J7VQ=");
    with_tvm_code("wallet3-r1",
                  "te6ccgEBAQEAYgAAwP8AIN0gggFMl7qXMO1E0NcLH+Ck8mCDCNcYINMf0x/TH/gjE7vyY+1E0NMf0x/T/"
                  "9FRMrryoVFEuvKiBPkBVBBV+RDyo/gAkyDXSpbTB9QC+wDo0QGkyMsfyx/L/8ntVA==");
    with_tvm_code("wallet3-r2",
                  "te6ccgEBAQEAcQAA3v8AIN0gggFMl7ohggEznLqxn3Gw7UTQ0x/THzHXC//jBOCk8mCDCNcYINMf0x/TH/gjE7vyY+1E0NMf0x/"
                  "T/9FRMrryoVFEuvKiBPkBVBBV+RDyo/gAkyDXSpbTB9QC+wDo0QGkyMsfyx/L/8ntVA==");
    auto check_revision = [&](td::Slice name, td::int32 default_revision) {
      auto it = map.find(name);
      CHECK(it != map.end());
      auto other_it = map.find(PSLICE() << name << "-r" << default_revision);
      CHECK(other_it != map.end());
      CHECK(it->second->get_hash() == other_it->second->get_hash());
    };
    check_revision("highload-wallet", HIGHLOAD_WALLET_REVISION);
    check_revision("highload-wallet-v2", HIGHLOAD_WALLET2_REVISION);

    //check_revision("simple-wallet", WALLET_REVISION);
    //check_revision("wallet", WALLET2_REVISION);
    //check_revision("wallet3", WALLET3_REVISION);
    return map;
  }();
  return map;
}
}  // namespace

td::Result<td::Ref<vm::Cell>> SmartContractCode::load(td::Slice name) {
  auto& map = get_map();
  auto it = map.find(name);
  if (it == map.end()) {
    return td::Status::Error(PSLICE() << "Can't load td::Ref<vm::Cell> " << name);
  }
  return it->second;
}
td::Ref<vm::Cell> SmartContractCode::multisig() {
  auto res = load("multisig").move_as_ok();
  return res;
}
td::Ref<vm::Cell> SmartContractCode::wallet3(int revision) {
  if (revision == 0) {
    revision = WALLET3_REVISION;
  }
  auto res = load(PSLICE() << "wallet3-r" << revision).move_as_ok();
  return res;
}
td::Ref<vm::Cell> SmartContractCode::wallet(int revision) {
  if (revision == 0) {
    revision = WALLET2_REVISION;
  }
  auto res = load(PSLICE() << "wallet-r" << revision).move_as_ok();
  return res;
}
td::Ref<vm::Cell> SmartContractCode::simple_wallet(int revision) {
  if (revision == 0) {
    revision = WALLET_REVISION;
  }
  auto res = load(PSLICE() << "simple-wallet-r" << revision).move_as_ok();
  return res;
}
td::Ref<vm::Cell> SmartContractCode::simple_wallet_ext() {
  static auto res = load("simple-wallet-ext").move_as_ok();
  return res;
}
td::Ref<vm::Cell> SmartContractCode::highload_wallet(int revision) {
  if (revision == 0) {
    revision = HIGHLOAD_WALLET_REVISION;
  }
  auto res = load(PSLICE() << "highload-wallet-r" << revision).move_as_ok();
  return res;
}
td::Ref<vm::Cell> SmartContractCode::highload_wallet_v2(int revision) {
  if (revision == 0) {
    revision = HIGHLOAD_WALLET2_REVISION;
  }
  auto res = load(PSLICE() << "highload-wallet-v2-r" << revision).move_as_ok();
  return res;
}
td::Ref<vm::Cell> SmartContractCode::dns_manual() {
  static auto res = load("dns-manual").move_as_ok();
  return res;
}
}  // namespace ton
