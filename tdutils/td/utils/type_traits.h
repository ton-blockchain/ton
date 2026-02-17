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

#include <cstddef>
#include <type_traits>

namespace td {

template <class FunctionT>
struct member_function_class;

template <class ReturnType, class Type, class... Args>
struct member_function_class<ReturnType (Type::*)(Args...)> {
  using type = Type;
  static constexpr size_t argument_count() {
    return sizeof...(Args);
  }
};

template <class FunctionT>
using member_function_class_t = typename member_function_class<FunctionT>::type;

template <class FunctionT>
constexpr size_t member_function_argument_count() {
  return member_function_class<FunctionT>::argument_count();
}

template <typename...>
struct TypeList {};

namespace detail {

template <typename T, template <typename...> typename Template>
struct IsSpecializationOfHelper : std::false_type {};

template <template <typename...> typename Template, typename... Args>
struct IsSpecializationOfHelper<Template<Args...>, Template> : std::true_type {};

template <typename, typename>
struct ConcatHelper {};

template <typename... List1, typename... List2>
struct ConcatHelper<TypeList<List1...>, TypeList<List2...>> {
  using type = TypeList<List1..., List2...>;
};

template <typename, typename>
struct InHelper {};

template <typename T, typename... Ts>
struct InHelper<T, TypeList<Ts...>> {
  constexpr static bool value = (std::is_same_v<T, Ts> || ...);
};

}  // namespace detail

template <typename T, template <typename...> typename Template>
concept IsSpecializationOf =
    detail::IsSpecializationOfHelper<std::remove_cv_t<std::remove_reference_t<T>>, Template>::value;

template <IsSpecializationOf<TypeList> List1, IsSpecializationOf<TypeList> List2>
using Concat = typename detail::ConcatHelper<List1, List2>::type;

template <typename T, typename List>
concept In = detail::InHelper<T, List>::value;

template <typename T, typename... Ts>
concept OneOf = (std::is_same_v<T, Ts> || ...);

}  // namespace td
