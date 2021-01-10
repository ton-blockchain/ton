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
#include "validator/validator.h"
#include "adnl/adnl.h"
#include "rldp/rldp.h"

namespace ton {

namespace validator {

class ValidatorManagerFactory {
 public:
  static td::actor::ActorOwn<ValidatorManagerInterface> create(td::Ref<ValidatorManagerOptions> opts,
                                                               std::string db_root,
                                                               td::actor::ActorId<keyring::Keyring> keyring,
                                                               td::actor::ActorId<adnl::Adnl> adnl,
                                                               td::actor::ActorId<rldp::Rldp> rldp,
                                                               td::actor::ActorId<overlay::Overlays> overlays);
};

}  // namespace validator

}  // namespace ton
