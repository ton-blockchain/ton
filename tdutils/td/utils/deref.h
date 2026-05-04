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

#include <concepts>
#include <utility>

namespace td {

template <typename T>
class DerefWrapper {
 private:
  T* ptr_;

 public:
  DerefWrapper(T* ptr) : ptr_(ptr) {
  }

  template <typename U>
    requires std::convertible_to<decltype(std::declval<U>().get()), T*>
  DerefWrapper(U&& ref) : ptr_(std::forward<U>(ref).get()) {
  }

  T* get() const {
    return ptr_;
  }

  T& operator*() const {
    return *ptr_;
  }

  T* operator->() const {
    return ptr_;
  }

  explicit operator bool() const {
    return ptr_;
  }

  bool operator==(const DerefWrapper& other) const
    requires std::constructible_from<bool, decltype(std::declval<T&>() == std::declval<T&>())>
  {
    return ptr_ == other.ptr_ || (ptr_ && other.ptr_ && *ptr_ == *other.ptr_);
  }
};

namespace detail {

template <typename T>
T deref_helper2(T*);

template <typename T>
T deref_helper(T*);

template <typename T>
decltype(deref_helper2(std::declval<T>().get())) deref_helper(T&&);

}  // namespace detail

template <typename T>
auto deref(T&& ref) {
  return DerefWrapper<decltype(detail::deref_helper(std::declval<T>()))>(std::forward<T>(ref));
}

}  // namespace td
