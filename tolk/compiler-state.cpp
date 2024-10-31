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
#include "compiler-state.h"

namespace tolk {

std::string tolk_version{"0.5.0"};

CompilerState G; // the only mutable global variable in tolk internals

void GlobalPragma::enable(SrcLocation loc) {
  if (deprecated_from_v_) {
    loc.show_warning(PSTRING() << "#pragma " << name_ <<
                     " is deprecated since Tolk v" << deprecated_from_v_ <<
                     ". Please, remove this line from your code.");
    return;
  }
  if (!loc.get_src_file()->is_entrypoint_file()) {
    // todo generally it's not true; rework pragmas completely
    loc.show_warning(PSTRING() << "#pragma " << name_ <<
                     " should be used in the main file only.");
  }

  enabled_ = true;
}

void GlobalPragma::always_on_and_deprecated(const char *deprecated_from_v) {
  deprecated_from_v_ = deprecated_from_v;
  enabled_ = true;
}


} // namespace tolk
