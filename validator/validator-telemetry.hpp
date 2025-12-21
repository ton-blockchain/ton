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
#include "adnl/adnl.h"
#include "interfaces/shard.h"
#include "td/actor/actor.h"

#include "overlay.h"

namespace ton::validator {
class ValidatorManager;

class ValidatorTelemetry : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual ~Callback() = default;
    virtual void send_telemetry(tl_object_ptr<ton_api::validator_telemetry> telemetry) = 0;
  };

  ValidatorTelemetry(adnl::AdnlNodeIdShort local_id, std::unique_ptr<Callback> callback)
      : local_id_(local_id), callback_(std::move(callback)) {
  }

  void start_up() override;
  void alarm() override;

 private:
  adnl::AdnlNodeIdShort local_id_;
  std::unique_ptr<Callback> callback_;

  std::string node_version_;
  std::string os_version_;
  td::uint32 cpu_cores_ = 0;
  td::uint64 ram_size_ = 0;

  td::Timestamp send_telemetry_at_ = td::Timestamp::never();

  void send_telemetry();

  static constexpr double PERIOD = 600.0;
};
}  // namespace ton::validator
