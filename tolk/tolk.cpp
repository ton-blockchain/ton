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
#include "pipeline.h"
#include "compiler-state.h"
#include "compilation-errors.h"
#include "lexer.h"
#include "type-system.h"
#include <sstream>

namespace tolk {

void define_builtins();

TolkCompilationResult tolk_proceed(const std::string &entrypoint_filename) {
  type_system_init();
  define_builtins();
  lexer_init();

  // on any error, an exception is thrown, and the message is printed out below
  // (currently, only a single error can be printed)
  try {
    pipeline_discover_and_parse_sources("@stdlib/common.tolk", entrypoint_filename);

    pipeline_register_global_symbols();
    pipeline_resolve_identifiers_and_assign_symbols();
    pipeline_resolve_types_and_aliases();
    pipeline_calculate_rvalue_lvalue();
    pipeline_infer_types_and_calls_and_fields();
    pipeline_check_inferred_types();
    pipeline_refine_lvalue_for_mutate_arguments();
    pipeline_check_rvalue_lvalue();
    pipeline_check_private_fields_usage();
    pipeline_check_pure_impure_operations();
    pipeline_check_constant_expressions();
    pipeline_mini_borrow_checker_for_mutate();
    pipeline_optimize_boolean_expressions();
    pipeline_detect_inline_in_place();
    pipeline_check_serialized_fields();
    pipeline_lazy_load_insertions();
    pipeline_transform_onInternalMessage();
    pipeline_convert_ast_to_legacy_Expr_Op();

    pipeline_find_unused_symbols();

    std::ostringstream os_fif;
    pipeline_generate_fif_output(os_fif);

    return TolkCompilationResult{
      .errors = {},
      .fatal_msg = "",
      .fift_code = os_fif.str(),
    };

  } catch (const Fatal& fatal) {
    return TolkCompilationResult{
      .errors = {},
      .fatal_msg = fatal.message,
      .fift_code = "",
    };

  } catch (const ThrownParseError& error) {
    return TolkCompilationResult{
      .errors = {error},
      .fatal_msg = "",
      .fift_code = "",
    };

  } catch (const UnexpectedASTNodeKind& error) {
    return TolkCompilationResult{
      .errors = {},
      .fatal_msg = error.message,
      .fift_code = "",
    };
  }
}

}  // namespace tolk
