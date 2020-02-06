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
#include "vm/cells.h"

namespace ton {
class SmartContractCode {
 public:
  static td::Result<td::Ref<vm::Cell>> load(td::Slice name);
  static td::Ref<vm::Cell> multisig();
  static td::Ref<vm::Cell> wallet3(int revision = 0);
  static td::Ref<vm::Cell> wallet(int revision = 0);
  static td::Ref<vm::Cell> simple_wallet(int revision = 0);
  static td::Ref<vm::Cell> simple_wallet_ext();
  static td::Ref<vm::Cell> highload_wallet(int revision = 0);
  static td::Ref<vm::Cell> highload_wallet_v2(int revision = 0);
  static td::Ref<vm::Cell> dns_manual();
};
}  // namespace ton
