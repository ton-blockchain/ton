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
*/
#pragma once

#include <utility>

namespace tolk {

// RecursionGuard is used to detect when a type/expansion circularly references itself.
// Example:
// > if (called_stack.includes(fun_ref)) fire();
// > called_stack.push_back(fun_ref);
// > RecursionGuard guard([&] {
// >   called_stack.pop_back();
// > });
// > analyze(fun_ref); // may enter the same execution point
// We intentionally use the destructor to roll the state back on potential `err(...).fire()` inside.
template<typename F>
class RecursionGuard {
  F on_destroy;

public:
  explicit RecursionGuard(F on_destroy)
    : on_destroy(std::move(on_destroy)) {}

  RecursionGuard(const RecursionGuard&) = delete;
  RecursionGuard& operator=(const RecursionGuard&) = delete;

  ~RecursionGuard() {
    on_destroy();
  }
};

} // namespace tolk
