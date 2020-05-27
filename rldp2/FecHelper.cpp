#include "FecHelper.h"

#include "td/utils/check.h"

namespace ton {
namespace rldp2 {
td::uint32 FecHelper::get_fec_symbols_count() const {
  constexpr td::uint32 x = 5;
  constexpr td::uint32 y = 5;
  // smallest (symbols_count + x + y * i) >  received_symbols_count
  if (symbols_count + x > received_symbols_count) {
    return symbols_count + x;
  }
  td::uint32 i = (received_symbols_count - (symbols_count + x)) / y + 1;
  return symbols_count + x + i * y;
}

td::uint32 FecHelper::get_left_fec_symbols_count() const {
  auto fec_symbols_count = get_fec_symbols_count();
  CHECK(fec_symbols_count > received_symbols_count);
  return fec_symbols_count - received_symbols_count;
}
}  // namespace rldp2
}  // namespace ton
