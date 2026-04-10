#pragma once

#include <atomic>
#include <compare>

namespace td {

namespace detail {

template <typename Tag>
inline std::atomic<unsigned> g_id = 0;

template <typename Tag>
class TypeRegistryHelper {
 public:
  class Id {
   public:
    std::strong_ordering operator<=>(const Id& other) const = default;

   private:
    friend TypeRegistryHelper;

    Id(unsigned value) : value_(value) {
    }

    unsigned value_;
  };

  TypeRegistryHelper() : id_((g_id<Tag>)++) {
  }

  Id id() {
    return id_;
  }

 private:
  Id id_;
};

}  // namespace detail

template <typename Tag>
using IdType = detail::TypeRegistryHelper<Tag>::Id;

template <typename Tag, typename T>
IdType<Tag> get_type_id() {
  static detail::TypeRegistryHelper<Tag> counter;
  return counter.id();
}

}  // namespace td
