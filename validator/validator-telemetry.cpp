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
#include "validator-telemetry.hpp"
#include "git.h"
#include "td/utils/Random.h"
#include "td/utils/port/uname.h"
#include "interfaces/validator-manager.h"

namespace ton::validator {

void ValidatorTelemetry::start_up() {
  node_version_ = PSTRING() << "validator-engine, Commit: " << GitMetadata::CommitSHA1()
                            << ", Date: " << GitMetadata::CommitDate();

  os_version_ = td::get_operating_system_version().str();

  auto r_total_mem_stat = td::get_total_mem_stat();
  if (r_total_mem_stat.is_error()) {
    LOG(WARNING) << "Cannot get RAM size: " << r_total_mem_stat.move_as_error();
  } else {
    ram_size_ = r_total_mem_stat.ok().total_ram;
  }

  auto r_cpu_cores = td::get_cpu_cores();
  if (r_cpu_cores.is_error()) {
    LOG(WARNING) << "Cannot get CPU info: " << r_cpu_cores.move_as_error();
  } else {
    cpu_cores_ = r_cpu_cores.move_as_ok();
  }

  LOG(DEBUG) << "Initializing validator telemetry, key = " << key_ << ", adnl_id = " << local_id_;
  alarm_timestamp().relax(send_telemetry_at_ = td::Timestamp::in(td::Random::fast(30.0, 60.0)));
}

void ValidatorTelemetry::alarm() {
  if (send_telemetry_at_.is_in_past()) {
    send_telemetry_at_ = td::Timestamp::never();
    send_telemetry();
  }
  alarm_timestamp().relax(send_telemetry_at_);
}

void ValidatorTelemetry::send_telemetry() {
  send_telemetry_at_ = td::Timestamp::in(PERIOD);

  auto telemetry = create_tl_object<ton_api::validator_telemetry>();
  telemetry->flags_ = 0;
  telemetry->timestamp_ = td::Clocks::system();
  telemetry->adnl_id_ = local_id_.bits256_value();
  telemetry->node_version_ = node_version_;
  telemetry->os_version_ = os_version_;
  telemetry->node_started_at_ = adnl::Adnl::adnl_start_time();
  telemetry->ram_size_ = ram_size_;
  telemetry->cpu_cores_ = cpu_cores_;
  telemetry->node_threads_ = (td::int32)td::actor::SchedulerContext::get()
                                 ->scheduler_group()
                                 ->schedulers.at(td::actor::SchedulerContext::get()->get_scheduler_id().value())
                                 .cpu_threads_count;

  LOG(DEBUG) << "Sending validator telemetry for adnl id " << local_id_;
  td::actor::send_closure(manager_, &ValidatorManager::send_validator_telemetry, key_, std::move(telemetry));
}

}  // namespace ton::validator
