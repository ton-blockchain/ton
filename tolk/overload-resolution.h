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

#include <vector>
#include "fwd-declarations.h"
#include "generics-helpers.h"

namespace tolk {

// when there are many methods with the same name, the overload resolution mechanism
// analyzes possible candidates to resolve the best match
struct MethodCallCandidate {
  TypePtr original_receiver;
  TypePtr instantiated_receiver;
  FunctionPtr method_ref;
  GenericsSubstitutions substitutedTs;

  MethodCallCandidate(TypePtr original_receiver, TypePtr instantiated_receiver, FunctionPtr method_ref, GenericsSubstitutions&& substitutedTs)
    : original_receiver(original_receiver)
    , instantiated_receiver(instantiated_receiver)
    , method_ref(method_ref)
    , substitutedTs(std::move(substitutedTs)) {}

  bool is_generic() const { return original_receiver != instantiated_receiver; }
};


std::vector<MethodCallCandidate> resolve_methods_for_call(TypePtr provided_receiver, std::string_view called_name);

} // namespace tolk
