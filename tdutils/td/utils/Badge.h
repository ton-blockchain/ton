#pragma once

namespace td {

template <typename T>
class Badge {
 public:
  constexpr Badge(Badge&&) = default;
  constexpr Badge& operator=(Badge&&) = default;

  Badge(const Badge&) = delete;
  Badge& operator=(const Badge&) = delete;

 private:
  friend T;

  constexpr Badge() = default;
};

}  // namespace td
