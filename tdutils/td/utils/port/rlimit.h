#pragma once

#include "td/utils/port/config.h"
#include "td/utils/port/platform.h"
#include "td/utils/Status.h"

namespace td {

enum class RlimitType { nofile, rss };

td::Status change_rlimit(RlimitType rlim_type, td::uint64 value, td::uint64 cap = 0);
td::Status change_maximize_rlimit(RlimitType rlim, td::uint64 value);

}  // namespace td
