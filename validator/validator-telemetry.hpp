/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.
*/
#pragma once
#include "overlay.h"
#include "td/actor/actor.h"
#include "adnl/adnl.h"
#include "interfaces/shard.h"

namespace ton::validator {
class ValidatorManager;

class ValidatorTelemetry : public td::actor::Actor {
public:
  ValidatorTelemetry(PublicKeyHash key, adnl::AdnlNodeIdShort local_id, td::Bits256 zero_state_file_hash,
                     td::actor::ActorId<ValidatorManager> manager)
    : key_(key)
    , local_id_(local_id)
    , zero_state_file_hash_(zero_state_file_hash)
    , manager_(std::move(manager)) {
  }

  void start_up() override;
  void alarm() override;

private:
  PublicKeyHash key_;
  adnl::AdnlNodeIdShort local_id_;
  td::Bits256 zero_state_file_hash_;
  td::actor::ActorId<ValidatorManager> manager_;

  std::string node_version_;
  std::string os_version_;
  td::uint32 cpu_cores_ = 0;
  td::uint64 ram_size_ = 0;

  td::Timestamp send_telemetry_at_ = td::Timestamp::never();

  void send_telemetry();

  static constexpr double PERIOD = 600.0;
  static constexpr td::uint32 MAX_SIZE = 8192;
};
} // namespace ton::validator