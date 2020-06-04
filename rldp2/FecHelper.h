#pragma once

#include "td/utils/int_types.h"

namespace ton {
namespace rldp2 {

struct FecHelper {
  td::uint32 symbols_count{0};
  td::uint32 received_symbols_count{0};

  td::uint32 get_fec_symbols_count() const;
  td::uint32 get_left_fec_symbols_count() const;
};

}  // namespace rldp2
}  // namespace ton
