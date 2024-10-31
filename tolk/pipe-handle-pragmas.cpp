/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.
*/
#include "tolk.h"
#include "src-file.h"
#include "ast.h"
#include "compiler-state.h"
#include "td/utils/misc.h"

namespace tolk {

static void handle_pragma_no_arg(V<ast_pragma_no_arg> v) {
  std::string_view pragma_name = v->pragma_name;
  if (pragma_name == G.pragma_allow_post_modification.name()) {
    G.pragma_allow_post_modification.enable(v->loc);
  } else if (pragma_name == G.pragma_compute_asm_ltr.name()) {
    G.pragma_compute_asm_ltr.enable(v->loc);
  } else if (pragma_name == G.pragma_remove_unused_functions.name()) {
    G.pragma_remove_unused_functions.enable(v->loc);
  } else {
    v->error("unknown pragma name");
  }
}

static void handle_pragma_version(V<ast_pragma_version> v) {
  char op = '=';
  bool eq = false;
  TokenType cmp_tok = v->cmp_tok;
  if (cmp_tok == tok_gt || cmp_tok == tok_geq) {
    op = '>';
    eq = cmp_tok == tok_geq;
  } else if (cmp_tok == tok_lt || cmp_tok == tok_leq) {
    op = '<';
    eq = cmp_tok == tok_leq;
  } else if (cmp_tok == tok_eq) {
    op = '=';
  } else if (cmp_tok == tok_bitwise_xor) {
    op = '^';
  } else {
    v->error("invalid comparison operator");
  }
  std::string_view pragma_value = v->semver;
  int sem_ver[3] = {0, 0, 0};
  char segs = 1;
  auto stoi = [&](std::string_view s) {
    auto R = td::to_integer_safe<int>(static_cast<std::string>(s));
    if (R.is_error()) {
      v->error("invalid semver format");
    }
    return R.move_as_ok();
  };
  std::istringstream iss_value(static_cast<std::string>(pragma_value));
  for (int idx = 0; idx < 3; idx++) {
    std::string s{"0"};
    std::getline(iss_value, s, '.');
    sem_ver[idx] = stoi(s);
  }
  // End reading semver from source code
  int tolk_ver[3] = {0, 0, 0};
  std::istringstream iss(tolk_version);
  for (int idx = 0; idx < 3; idx++) {
    std::string s;
    std::getline(iss, s, '.');
    tolk_ver[idx] = stoi(s);
  }
  // End parsing embedded semver
  bool match = true;
  switch (op) {
    case '=':
      if ((tolk_ver[0] != sem_ver[0]) || (tolk_ver[1] != sem_ver[1]) || (tolk_ver[2] != sem_ver[2])) {
        match = false;
      }
      break;
    case '>':
      if (((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] == sem_ver[1]) && (tolk_ver[2] == sem_ver[2]) && !eq) ||
          ((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] == sem_ver[1]) && (tolk_ver[2] < sem_ver[2])) ||
          ((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] < sem_ver[1])) || ((tolk_ver[0] < sem_ver[0]))) {
        match = false;
      }
      break;
    case '<':
      if (((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] == sem_ver[1]) && (tolk_ver[2] == sem_ver[2]) && !eq) ||
          ((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] == sem_ver[1]) && (tolk_ver[2] > sem_ver[2])) ||
          ((tolk_ver[0] == sem_ver[0]) && (tolk_ver[1] > sem_ver[1])) || ((tolk_ver[0] > sem_ver[0]))) {
        match = false;
      }
      break;
    case '^':
      if (((segs == 3) &&
           ((tolk_ver[0] != sem_ver[0]) || (tolk_ver[1] != sem_ver[1]) || (tolk_ver[2] < sem_ver[2]))) ||
          ((segs == 2) && ((tolk_ver[0] != sem_ver[0]) || (tolk_ver[1] < sem_ver[1]))) ||
          ((segs == 1) && ((tolk_ver[0] < sem_ver[0])))) {
        match = false;
      }
      break;
    default:
      tolk_assert(false);
  }
  if (!match) {
    v->error("Tolk version " + tolk_version + " does not satisfy this condition");
  }
}

void pipeline_handle_pragmas(const AllSrcFiles& all_src_files) {
  for (const SrcFile* file : all_src_files) {
    tolk_assert(file->ast);

    for (AnyV v : file->ast->as<ast_tolk_file>()->get_toplevel_declarations()) {
      if (auto v_no_arg = v->try_as<ast_pragma_no_arg>()) {
        handle_pragma_no_arg(v_no_arg);
      } else if (auto v_version = v->try_as<ast_pragma_version>()) {
        handle_pragma_version(v_version);
      }
    }
  }
}

} // namespace tolk
