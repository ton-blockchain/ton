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

#include "td/utils/logging.h"

#define VM_LOG_IMPL(st, mask)                                                             \
  LOG_IMPL_FULL(get_log_interface(st), get_log_options(st), DEBUG, VERBOSITY_NAME(DEBUG), \
                (get_log_mask(st) & mask) != 0, "")

#define VM_LOG(st) VM_LOG_IMPL(st, 1)
#define VM_LOG_MASK(st, mask) VM_LOG_IMPL(st, mask)

namespace vm {
struct VmLog {
  td::LogInterface *log_interface{td::log_interface};
  td::LogOptions log_options{td::log_options};
  enum { DumpStack = 2, ExecLocation = 4, GasRemaining = 8, DumpStackVerbose = 16, DumpC5 = 32 };
  int log_mask{1};
  static VmLog Null() {
    VmLog res;
    res.log_options.level = 0;
    res.log_mask = 0;
    return res;
  }
};

inline VmLog make_vm_log(td::LogInterface *log_interface, int vm_log_verbosity, bool dump_c5 = false) {
  if (vm_log_verbosity < 0) {
    return VmLog::Null();
  }

  VmLog log{log_interface ? log_interface : td::log_interface, td::LogOptions(VERBOSITY_NAME(DEBUG), true, false)};
  if (vm_log_verbosity > 1) {
    log.log_mask |= VmLog::ExecLocation;
    if (vm_log_verbosity > 2) {
      log.log_mask |= VmLog::GasRemaining;
      if (vm_log_verbosity > 3) {
        log.log_mask |= VmLog::DumpStack;
        if (vm_log_verbosity > 4) {
          log.log_mask |= VmLog::DumpStackVerbose;
          if (dump_c5) {
            log.log_mask |= VmLog::DumpC5;
          }
        }
      }
    }
  }
  return log;
}

template <class State>
td::LogInterface &get_log_interface(State *st) {
  return st ? *st->get_log().log_interface : *::td::log_interface;
}

template <class State>
auto get_log_options(State *st) {
  return st ? st->get_log().log_options : ::td::log_options;
}

template <class State>
auto get_log_mask(State *st) {
  return st ? st->get_log().log_mask : 1;
}

}  // namespace vm
